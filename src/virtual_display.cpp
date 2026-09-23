/**
 * @file src/virtual_display.cpp
 * @brief Definitions for streaming a display created for the session.
 */
// header include
#include "virtual_display.h"

// standard includes
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

// local includes
#include "config.h"
#include "display_device.h"
#include "logging.h"
#include "rtsp.h"

using namespace std::literals;

namespace virtual_display {

  namespace {

    /**
     * @brief How many pings in a row may fail before the driver is given up on.
     *
     * One lost ping is not worth ending a session for. Several in a row means
     * the driver is about to drop the display underneath us.
     */
    constexpr unsigned max_ping_failures = 3;

    /**
     * @brief The one thread allowed to touch a driver.
     *
     * A device call cannot be cancelled: once it is in the driver it comes
     * back when it comes back, or never. Bounding each call separately and
     * abandoning the thread was worse than the problem, because a call that
     * timed out would still be in flight while the next one, or the cleanup
     * for it, ran against the same handle.
     *
     * So every call goes through one thread and calls never overlap. A caller
     * that gives up leaves its call running, and because the worker is then
     * still busy nothing else is submitted: the owner marks the driver
     * poisoned instead. If the call does eventually return, the worker goes
     * idle and the driver can be picked up again from a fresh handle.
     */
    class worker_t {
    public:
      worker_t():
          m_state {std::make_shared<state_t>()},
          m_thread {[state = m_state] {
            run(state);
          }} {
      }

      ~worker_t() {
        {
          std::lock_guard lock {m_state->mutex};
          m_state->stopping = true;
        }
        m_state->wake.notify_all();
        m_state->idle_cv.notify_all();

        if (m_state->busy) {
          // A call is still inside the driver. Joining would wait for exactly
          // the thing this class exists not to wait for, so the thread is left
          // to finish. It only ever touches state it shares ownership of, so
          // it stays safe once this object is gone.
          m_thread.detach();
        } else {
          m_thread.join();
        }
      }

      worker_t(const worker_t &) = delete;
      worker_t &operator=(const worker_t &) = delete;

      /**
       * @brief Whether a call submitted earlier has still not come back.
       */
      [[nodiscard]] bool busy() const {
        return m_state->busy;
      }

      /**
       * @brief Run something on the worker and wait for it.
       *
       * Calls never overlap. A call another caller is still waiting on is
       * waited for, since it answers in its own time: the heartbeat pings
       * while a session resizes, and one call arriving during the other is
       * no reason to think the driver has stopped answering. A call its
       * caller gave up on may never answer, so that one is not waited for.
       *
       * @param work What to run. Must capture by value: the caller can walk
       *             away from it, so nothing of the caller's may be referenced.
       * @param deadline How long to wait for the worker to be free, and then
       *                 as long again for the work.
       * @return The result, or nothing if a deadline passed or the worker is
       *         stuck on a call its caller gave up on.
       */
      template<class R>
      std::optional<R> run_bounded(std::function<R()> work, std::chrono::milliseconds deadline) {
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(work));
        auto result = task->get_future();

        // The task's own future is ready the moment the work returns, which is
        // before the worker has been marked idle again. Waiting on that would
        // let the caller come back and submit the next call while this one
        // still looks in flight, and the driver would be poisoned over a call
        // it had in fact answered. So the handover is a second future, set
        // after the worker is idle.
        auto finished = std::make_shared<std::promise<void>>();
        auto idle = finished->get_future();

        {
          std::unique_lock lock {m_state->mutex};
          const bool free = m_state->idle_cv.wait_for(lock, deadline, [this] {
            return m_state->stopping || !m_state->busy || m_state->abandoned;
          });
          if (!free || m_state->stopping || m_state->busy) {
            return std::nullopt;
          }
          m_state->busy = true;
          m_state->abandoned = false;
          m_state->job = [state = m_state, task, finished] {
            try {
              (*task)();
            } catch (...) {
              // Whatever the work threw is already in its future. Only the
              // task machinery itself can land here, and it must not leave
              // the worker marked busy for good.
            }
            {
              std::lock_guard lock {state->mutex};
              state->busy = false;
            }
            state->idle_cv.notify_all();
            finished->set_value();
          };
        }
        m_state->wake.notify_all();

        if (idle.wait_for(deadline) != std::future_status::ready) {
          // Given up on, so nobody queues behind it any more.
          {
            std::lock_guard lock {m_state->mutex};
            m_state->abandoned = true;
          }
          m_state->idle_cv.notify_all();
          return std::nullopt;
        }

        try {
          return result.get();
        } catch (const std::exception &ex) {
          // A driver call that threw told us nothing, so it is treated like
          // one that did not answer: the caller poisons the driver and keeps
          // whatever cleanup is owed.
          BOOST_LOG(error) << "Virtual display driver call threw: "sv << ex.what();
          return std::nullopt;
        } catch (...) {
          BOOST_LOG(error) << "Virtual display driver call threw"sv;
          return std::nullopt;
        }
      }

    private:
      /**
       * @brief Everything the thread touches, owned by the thread as well.
       */
      struct state_t {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle_cv;  ///< Signalled when the worker goes idle or a call is given up on.
        std::function<void()> job;
        bool stopping {false};
        std::atomic<bool> busy {false};
        bool abandoned {false};  ///< The call in flight was given up on by its caller.
      };

      static void run(std::shared_ptr<state_t> state) {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock lock {state->mutex};
            state->wake.wait(lock, [&state] {
              return state->stopping || state->job;
            });
            if (!state->job) {
              return;
            }
            job = std::exchange(state->job, nullptr);
          }
          job();
        }
      }

      std::shared_ptr<state_t> m_state;
      std::thread m_thread;
    };

    /**
     * @brief Keeps the driver aware that Sunshine is still here.
     *
     * The driver drops the displays of a client that goes quiet, so this runs
     * for as long as a lease is held.
     */
    struct heartbeat_t {
      std::chrono::milliseconds interval {};
      std::uint64_t generation {};
      std::function<bool()> ping;  ///< Already bounded, so this returns.
      std::function<void(std::uint64_t)> on_lost;

      std::mutex mutex;
      std::condition_variable wake;
      bool stop {false};

      void request_stop() {
        {
          std::lock_guard lock {mutex};
          stop = true;
        }
        wake.notify_all();
      }
    };

    /**
     * @brief Ping until told to stop, or until the driver stops answering.
     *
     * @param state Shared heartbeat state.
     */
    void run_heartbeat(std::shared_ptr<heartbeat_t> state) {
      unsigned failures = 0;

      for (;;) {
        {
          std::unique_lock lock {state->mutex};
          if (state->wake.wait_for(lock, state->interval, [&state] {
                return state->stop;
              })) {
            return;
          }
        }

        if (state->ping()) {
          failures = 0;
          continue;
        }

        if (++failures <= max_ping_failures) {
          BOOST_LOG(warning) << "Virtual display driver missed a heartbeat ("sv << failures << " in a row)"sv;
          continue;
        }

        BOOST_LOG(error) << "Virtual display driver stopped answering. The display it created is gone."sv;
        state->on_lost(state->generation);
        return;
      }
    }

  }  // namespace

  std::string to_string(error_e error) {
    switch (error) {
      case error_e::driver_unavailable:
        return "the virtual display driver is not installed or could not be opened";
      case error_e::protocol_mismatch:
        return "the virtual display driver speaks a protocol this build of Sunshine cannot use";
      case error_e::watchdog_unavailable:
        return "the virtual display driver did not report a watchdog, so a display could outlive Sunshine";
      case error_e::heartbeat_failed:
        return "the virtual display driver stopped answering";
      case error_e::create_failed:
        return "the virtual display driver refused to create a display";
      case error_e::not_ready:
        return "the virtual display was created but Windows did not bring it up";
      case error_e::mode_unavailable:
        return "the virtual display does not offer the resolution the client asked for";
      case error_e::already_leased:
        return "another session is already using the virtual display";
      case error_e::busy:
        return "the virtual display driver has not answered an earlier request yet";
      case error_e::not_leased:
        return "no session is streaming a virtual display";
    }
    return "unknown error";
  }

  /**
   * @brief Everything one manager owns, shared so a heartbeat can reach it.
   */
  struct manager_t::impl_t: std::enable_shared_from_this<manager_t::impl_t> {
    backend_factory_t make;  ///< Makes a driver backend when one is needed.
    device_id_lookup_t device_id_lookup;  ///< Turns a platform display name into the id Sunshine uses.
    timeouts_t timeouts;  ///< How long to wait for things that are not instant.

    mutable std::mutex mutex;  ///< Guards everything below it.
    state_e state {state_e::idle};  ///< What the lease is doing.
    std::shared_ptr<backend_t> backend;  ///< The driver, once one has been opened.
    std::shared_ptr<worker_t> worker;  ///< The only thread allowed to touch that driver.
    uuid_util::uuid_t id {};  ///< Identifies the display this lease created.

    /**
     * @brief Whether a display may exist under id, and so has to be removed.
     *
     * Set the moment a create is submitted rather than when one succeeds. A
     * create that never came back may well have made a display, and the
     * identifier for it is known, so the obligation to remove it outlives
     * being unable to.
     */
    bool display_may_exist {false};
    display_t display {};  ///< The display being streamed, while one is held.
    std::uint64_t generation {0};  ///< Changes with every lease taken.
    std::shared_ptr<heartbeat_t> heartbeat;  ///< Keeps the driver aware of us while a lease is held.
    std::function<void()> fault_handler;  ///< What to run when the driver stops answering.
    bool fault_reported {false};  ///< One report per lease, however it is noticed.

    std::string client_name;  ///< Stamped into every display the lease creates.
    std::string client_uid;  ///< Likewise.

    /**
     * @brief The lease's other identity, which a display replacing the leased one is created under.
     *
     * Once the replacement is streamed the two swap. However many times a
     * session resizes, Windows only ever sees two monitors for it.
     */
    uuid_util::uuid_t alt_id {};

    /**
     * @brief Whether a display may exist under alt_id, and so has to be removed.
     *
     * Set before a replacement is asked for, like display_may_exist. Also
     * set for an old display a resize could not remove, which is why another
     * replacement first has to get rid of whatever is there.
     */
    bool replacement_may_exist {false};
    display_t replacement {};  ///< The replacement, once Windows knows it.
    bool replacement_in_use {false};  ///< Whether output_override() names the replacement.

    /**
     * @brief Ask the driver something, and poison it if it does not answer.
     *
     * @param work What to ask. Must capture by value.
     * @return The answer, or nothing if the driver did not give one in time.
     */
    template<class R>
    std::optional<R> call(std::function<R()> work) {
      // Kept alive for the call: a recovery on another thread may drop the
      // manager's reference to it while this one is still using it.
      std::shared_ptr<worker_t> worker_ref;
      {
        std::lock_guard lock {mutex};
        worker_ref = worker;
      }
      if (!worker_ref) {
        return std::nullopt;
      }

      auto answer = worker_ref->run_bounded<R>(std::move(work), timeouts.call);
      if (!answer) {
        // The call may still be inside the driver, so nothing else may be
        // asked of it. Refusing until it comes back is the only way to keep a
        // later call from overtaking this one.
        std::lock_guard lock {mutex};
        state = state_e::poisoned;
      }
      return answer;
    }

    /**
     * @brief Finish what a driver that stopped answering left behind.
     *
     * Being poisoned is not permission to forget: the call that never came
     * back may have created a display, and this process knows the identifier
     * for it. So the handle and the identifier are kept, and when the overdue
     * call finally returns the display is removed and the handle closed,
     * in that order, before anything else may use the driver again.
     *
     * A driver that never answers stays poisoned, which keeps every session
     * refused rather than letting one start on top of an unknown state.
     *
     * Must be called without the lock held.
     */
    void try_recover() {
      std::shared_ptr<worker_t> worker_ref;
      std::shared_ptr<backend_t> backend_ref;
      uuid_util::uuid_t stale {};
      uuid_util::uuid_t stale_alt {};
      bool remove_first = false;
      bool remove_alt = false;

      {
        std::lock_guard lock {mutex};
        if (state != state_e::poisoned || !worker || worker->busy()) {
          // Not ours to do: either nothing is owed, someone else already
          // claimed it, or the driver is still holding the overdue call.
          return;
        }

        // Claimed, so two callers cannot both run the same cleanup.
        state = state_e::recovering;
        worker_ref = worker;
        backend_ref = backend;
        stale = id;
        stale_alt = alt_id;
        remove_first = display_may_exist;
        remove_alt = replacement_may_exist;
      }

      // Run on the same worker, in order, so the removal cannot overtake
      // whatever the driver was still doing. Both have to actually succeed:
      // a removal the driver refused leaves a display behind, and saying
      // otherwise would let the next session start on top of it.
      bool cleaned = true;
      if (backend_ref) {
        if (remove_alt) {
          const auto removed = worker_ref->run_bounded<bool>([backend_ref, stale_alt] {
            return backend_ref->remove(stale_alt);
          },
                                                             timeouts.call);
          cleaned = removed.value_or(false);
          if (cleaned) {
            std::lock_guard lock {mutex};
            replacement_may_exist = false;
          }
        }

        if (cleaned && remove_first) {
          const auto removed = worker_ref->run_bounded<bool>([backend_ref, stale] {
            return backend_ref->remove(stale);
          },
                                                             timeouts.call);
          cleaned = removed.value_or(false);
        }

        if (cleaned) {
          const auto closed = worker_ref->run_bounded<bool>([backend_ref] {
            backend_ref->close();
            return true;
          },
                                                            timeouts.call);
          cleaned = closed.value_or(false);
        }
      }

      std::lock_guard lock {mutex};
      if (state != state_e::recovering) {
        return;
      }

      if (!cleaned) {
        // Still owed. display_may_exist and the identifier stay as they are,
        // and no session may start until this comes good.
        BOOST_LOG(warning) << "The virtual display driver has not let go of what it was holding; "
                              "no session can use it yet."sv;
        state = state_e::poisoned;
        return;
      }

      BOOST_LOG(info) << "Virtual display driver answered again, and what it was holding is cleaned up"sv;
      worker.reset();
      backend.reset();
      heartbeat.reset();
      display = {};
      display_may_exist = false;
      replacement = {};
      replacement_may_exist = false;
      replacement_in_use = false;
      state = state_e::idle;
    }

    /**
     * @brief Whether the lease a caller started with is still the one held.
     *
     * Must be called without the lock held.
     *
     * @param expected The generation the caller started with.
     * @return True while that lease is active.
     */
    bool still_leased(std::uint64_t expected) const {
      std::lock_guard lock {mutex};
      return state == state_e::active && generation == expected;
    }

    /**
     * @brief Stop streaming a display that is no longer there.
     *
     * @param lost_generation Which lease the heartbeat belonged to.
     */
    void on_heartbeat_lost(std::uint64_t lost_generation) {
      std::function<void()> handler;
      {
        std::lock_guard lock {mutex};
        if (generation != lost_generation || fault_reported) {
          // A heartbeat belonging to a lease that has already ended, or one
          // whose loss has already been reported.
          return;
        }

        // Poisoned counts: a ping that never came back is how the driver
        // going away usually shows up, and the session streaming the display
        // still has to be told.
        if (state != state_e::active && state != state_e::poisoned) {
          return;
        }

        fault_reported = true;
        handler = fault_handler;
      }

      if (!handler) {
        BOOST_LOG(error) << "Nothing is handling the lost virtual display, so the session will keep "
                            "streaming a display that is gone"sv;
        return;
      }

      // Stopping the stream and restoring the display belongs to the owner,
      // and it comes back through release() once it is done.
      handler();
    }
  };

#ifndef _WIN32
  std::unique_ptr<backend_t> make_backend() {
    // No virtual display driver outside Windows. Every acquire then fails,
    // which is what the option's fail-closed contract requires.
    return nullptr;
  }
#endif

  manager_t::manager_t(backend_factory_t make, device_id_lookup_t device_id_lookup, timeouts_t timeouts):
      m_impl {std::make_shared<impl_t>()} {
    m_impl->make = std::move(make);
    m_impl->device_id_lookup = std::move(device_id_lookup);
    m_impl->timeouts = timeouts;
  }

  manager_t::~manager_t() {
    release();
  }

  std::variant<display_t, error_e> manager_t::acquire(
    const std::string &client_name,
    const std::string &client_uid,
    const mode_t &mode,
    const std::string &adapter_name
  ) {
    auto &impl = *m_impl;
    std::shared_ptr<backend_t> backend;

    // Anything an earlier driver left behind is finished first, and only a
    // driver that has been cleaned up is handed to a new session.
    impl.try_recover();

    // Reserved before anything is done, so two requests arriving together
    // cannot both find the lease free.
    {
      std::lock_guard lock {impl.mutex};

      switch (impl.state) {
        case state_e::idle:
          break;
        case state_e::poisoned:
        case state_e::recovering:
          return error_e::busy;
        default:
          return error_e::already_leased;
      }

      if (!impl.backend) {
        impl.backend = impl.make ? impl.make() : nullptr;
        if (!impl.backend) {
          return error_e::driver_unavailable;
        }
        impl.worker = std::make_shared<worker_t>();
      }

      backend = impl.backend;
      impl.state = state_e::provisioning;
      impl.fault_reported = false;
      impl.client_name = client_name;
      impl.client_uid = client_uid;
      impl.alt_id = uuid_util::uuid_t::generate();
      impl.replacement = {};
      impl.replacement_may_exist = false;
      impl.replacement_in_use = false;
    }

    // Copies, because a call the caller gives up on keeps running with them.
    const auto id = uuid_util::uuid_t::generate();
    const auto name = client_name;
    const auto uid = client_uid;
    const auto wanted = mode;
    const auto adapter = adapter_name;

    // Puts the lease back the way it was found. A display is only removed
    // when this process created one, and only by the identifier it created.
    const auto give_up = [&impl, backend, id](error_e error, bool created) {
      if (created) {
        impl.call<bool>([backend, id] {
          return backend->remove(id);
        });

        std::lock_guard lock {impl.mutex};
        // Removed, so nothing is owed for it any more. If the removal itself
        // did not come back, the poisoned recovery still has the identifier.
        if (impl.state != state_e::poisoned) {
          impl.display_may_exist = false;
        }
      }
      impl.call<bool>([backend] {
        backend->close();
        return true;
      });

      std::lock_guard lock {impl.mutex};
      if (impl.state == state_e::provisioning) {
        impl.state = state_e::idle;
      }
      return error;
    };

    if (!impl.call<bool>([backend] {
               return backend->open();
             })
           .value_or(false)) {
      return give_up(error_e::driver_unavailable, false);
    }

    if (!impl.call<bool>([backend] {
               return backend->protocol_supported();
             })
           .value_or(false)) {
      return give_up(error_e::protocol_mismatch, false);
    }

    const auto watchdog = impl.call<std::optional<std::chrono::seconds>>([backend] {
      return backend->watchdog_timeout();
    });
    if (!watchdog || !*watchdog || (*watchdog)->count() <= 0) {
      // Without a watchdog, a Sunshine that crashes leaves a display behind
      // with nothing able to remove it, so the lease is refused instead.
      return give_up(error_e::watchdog_unavailable, false);
    }

    // The driver has to answer before it is asked for anything, so a display
    // is never created for one that cannot be kept alive.
    if (!impl.call<bool>([backend] {
               return backend->ping();
             })
           .value_or(false)) {
      return give_up(error_e::heartbeat_failed, false);
    }

    if (!adapter.empty()) {
      impl.call<bool>([backend, adapter] {
        backend->set_render_adapter(adapter);
        return true;
      });
    }

    // A driver that accepted the request but did not describe the result may
    // still have created something, and the identifier is known either way,
    // so that case is removed rather than forgotten.
    // Noted before the request goes out, not after it comes back: a create
    // that never answers may still have made a display, and this is where
    // the identifier for it is recorded.
    {
      std::lock_guard lock {impl.mutex};
      impl.id = id;
      impl.display_may_exist = true;
    }

    const auto created = impl.call<creation_e>([backend, id, name, uid, wanted] {
                               return backend->add(id, name, uid, wanted);
                             })
                           .value_or(creation_e::created_unverifiable);

    if (created == creation_e::not_created) {
      std::lock_guard lock {impl.mutex};
      impl.display_may_exist = false;
    }

    if (created != creation_e::created) {
      return give_up(error_e::create_failed, created == creation_e::created_unverifiable);
    }

    auto last_state = resolution_t::readiness_e::not_ready;
    display_t display;

    const auto give_up_at = std::chrono::steady_clock::now() + impl.timeouts.readiness;
    for (;;) {
      const auto resolved = impl.call<resolution_t>([backend, id, wanted] {
        return backend->resolve(id, wanted);
      });

      if (resolved) {
        last_state = resolved->state;
        if (last_state == resolution_t::readiness_e::ready) {
          // The device list Sunshine addresses displays through can lag
          // behind Windows, so a miss here is another reason to wait.
          display = resolved->display;
          display.device_id = impl.device_id_lookup ? impl.device_id_lookup(display.gdi_name) : std::string {};
          if (!display.device_id.empty()) {
            break;
          }
          last_state = resolution_t::readiness_e::not_ready;
        }
      }

      if (std::chrono::steady_clock::now() >= give_up_at) {
        return give_up(
          last_state == resolution_t::readiness_e::mode_missing ? error_e::mode_unavailable : error_e::not_ready,
          true
        );
      }

      std::this_thread::sleep_for(impl.timeouts.readiness_poll);
    }

    auto heartbeat = std::make_shared<heartbeat_t>();
    // A third of the driver's patience, so two pings can be lost before it
    // decides Sunshine is gone.
    heartbeat->interval = std::chrono::duration_cast<std::chrono::milliseconds>(**watchdog) / 3;
    heartbeat->ping = [weak = m_impl->weak_from_this(), backend, generation = m_impl->generation + 1] {
      auto impl = weak.lock();
      if (!impl) {
        return false;
      }

      {
        // Nothing is asked of a driver that has not answered an earlier
        // call, or of one this lease no longer owns. Otherwise a late answer
        // could keep the driver's watchdog alive for a display nobody holds.
        std::lock_guard lock {impl->mutex};
        if (impl->state != state_e::active || impl->generation != generation) {
          return false;
        }
      }

      return impl->call<bool>([backend] {
                   return backend->ping();
                 })
        .value_or(false);
    };
    heartbeat->on_lost = [weak = m_impl->weak_from_this()](std::uint64_t generation) {
      if (auto impl = weak.lock()) {
        impl->on_heartbeat_lost(generation);
      }
    };

    bool stolen = false;
    {
      std::lock_guard lock {impl.mutex};
      if (impl.state != state_e::provisioning) {
        // Something tore the lease down while it was being set up. Decided
        // here, acted on below: give_up() takes this same lock.
        stolen = true;
      } else {
        impl.generation += 1;
        heartbeat->generation = impl.generation;
        impl.state = state_e::active;
        impl.display = display;
        impl.heartbeat = heartbeat;
      }
    }

    if (stolen) {
      return give_up(error_e::already_leased, true);
    }

    std::thread {run_heartbeat, heartbeat}.detach();

    BOOST_LOG(info) << "Streaming virtual display "sv << display.gdi_name
                    << " at "sv << wanted.width << 'x' << wanted.height
                    << " @ "sv << (wanted.refresh_rate_millihz / 1000.) << "Hz"sv;
    return display;
  }

  void manager_t::release() {
    std::ignore = release_impl(std::nullopt);
  }

  bool manager_t::release_generation(std::uint64_t generation) {
    return release_impl(generation);
  }

  /**
   * @brief Give the display back, optionally only if the lease is still the expected one.
   *
   * The check and the claim happen together. Doing them in separate critical
   * sections would let a lease change hands in between, and a restore that
   * outlived its lease would end the next one.
   *
   * @param expected_generation The lease the caller means to end, or nothing for whichever is current.
   * @return False only when the lease had already moved on.
   */
  bool manager_t::release_impl(std::optional<std::uint64_t> expected_generation) {
    auto &impl = *m_impl;

    std::shared_ptr<heartbeat_t> heartbeat;
    std::shared_ptr<backend_t> backend;
    uuid_util::uuid_t id {};
    uuid_util::uuid_t alt_id {};
    bool had_display = false;
    bool had_replacement = false;
    bool poisoned = false;

    {
      std::lock_guard lock {impl.mutex};
      if (expected_generation && *expected_generation != impl.generation) {
        return false;
      }

      if (impl.state == state_e::idle) {
        return true;
      }

      if (impl.state == state_e::reverting) {
        // Another caller is already giving this lease back. Joining in would
        // mean a second removal and a second close for one display.
        return true;
      }

      heartbeat = std::exchange(impl.heartbeat, nullptr);
      backend = impl.backend;
      id = impl.id;
      alt_id = impl.alt_id;

      if (impl.state == state_e::poisoned || impl.state == state_e::recovering) {
        // What this owes belongs to the recovery. Moving it to reverting
        // would take the cleanup away from the only thing that can do it.
        poisoned = true;
      } else {
        had_display = impl.state == state_e::active;
        had_replacement = had_display && impl.replacement_may_exist;
        impl.replacement_in_use = false;
        impl.state = state_e::reverting;
      }
    }

    if (heartbeat) {
      heartbeat->request_stop();
    }

    if (poisoned) {
      // Nothing may be asked of the driver until the call it has not answered
      // comes back. Tried now in case it already has.
      impl.try_recover();
      return true;
    }

    if (backend) {
      // A display a resize created, or one it could not get rid of, goes
      // first, under the same rule as the leased one below.
      if (had_replacement) {
        const auto removed = impl.call<bool>([backend, alt_id] {
          return backend->remove(alt_id);
        });

        if (!removed.value_or(false)) {
          BOOST_LOG(warning) << "The virtual display driver did not remove the display a resize created. "
                                "No session can use it until it does."sv;

          std::lock_guard lock {impl.mutex};
          if (impl.state == state_e::reverting) {
            impl.state = state_e::poisoned;
          }
          return true;
        }

        std::lock_guard lock {impl.mutex};
        impl.replacement_may_exist = false;
        impl.replacement = {};
      }

      // Removed while the lease still names it, so the display is never
      // unaccounted for while it still exists.
      if (had_display) {
        const auto removed = impl.call<bool>([backend, id] {
          return backend->remove(id);
        });

        if (!removed.value_or(false)) {
          // The display may well still be there. Calling this done would let
          // the next session start on top of it, so the lease keeps the
          // identifier and stays out of use until a removal succeeds.
          BOOST_LOG(warning) << "The virtual display driver did not remove the display. No session can "
                                "use it until it does."sv;

          std::lock_guard lock {impl.mutex};
          if (impl.state == state_e::reverting) {
            impl.state = state_e::poisoned;
          }
          return true;
        }

        std::lock_guard lock {impl.mutex};
        impl.display_may_exist = false;
      }

      impl.call<bool>([backend] {
        backend->close();
        return true;
      });
    }

    std::lock_guard lock {impl.mutex};
    impl.display = {};
    if (impl.state == state_e::reverting) {
      impl.display_may_exist = false;
      impl.state = state_e::idle;
    }
    return true;
  }

  bool manager_t::leased() const {
    std::lock_guard lock {m_impl->mutex};
    return m_impl->state == state_e::active;
  }

  state_e manager_t::state() const {
    std::lock_guard lock {m_impl->mutex};
    return m_impl->state;
  }

  std::uint64_t manager_t::generation() const {
    std::lock_guard lock {m_impl->mutex};
    return m_impl->generation;
  }

  void manager_t::set_fault_handler(std::function<void()> handler) {
    std::lock_guard lock {m_impl->mutex};
    m_impl->fault_handler = std::move(handler);
  }

  std::optional<std::string> manager_t::output_override() const {
    std::lock_guard lock {m_impl->mutex};
    if (m_impl->state != state_e::active) {
      return std::nullopt;
    }
    if (m_impl->replacement_in_use) {
      return m_impl->replacement.device_id;
    }
    return m_impl->display.device_id;
  }

  std::variant<display_t, error_e> manager_t::prepare_replacement(const mode_t &mode) {
    auto &impl = *m_impl;

    std::shared_ptr<backend_t> backend;
    uuid_util::uuid_t alt {};
    std::string name;
    std::string uid;
    std::uint64_t lease {};
    bool clear_first = false;
    {
      std::lock_guard lock {impl.mutex};
      switch (impl.state) {
        case state_e::active:
          break;
        case state_e::poisoned:
        case state_e::recovering:
          return error_e::busy;
        default:
          return error_e::not_leased;
      }

      backend = impl.backend;
      alt = impl.alt_id;
      name = impl.client_name;
      uid = impl.client_uid;
      lease = impl.generation;
      clear_first = impl.replacement_may_exist;
      impl.replacement = {};
      impl.replacement_in_use = false;
    }

    if (clear_first) {
      // Something may still be there under this identity: a replacement that
      // was never finished, or an old display a resize could not remove. It
      // has to go before the identity can be used again.
      if (!impl.call<bool>([backend, alt] {
                 return backend->remove(alt);
               })
             .value_or(false)) {
        return error_e::busy;
      }

      std::lock_guard lock {impl.mutex};
      if (impl.state == state_e::active && impl.generation == lease) {
        impl.replacement_may_exist = false;
      }
    }

    {
      // Owed removal before the request goes out, for the same reason as in
      // acquire(): a create that never answers may still have made a display.
      std::lock_guard lock {impl.mutex};
      if (impl.state != state_e::active || impl.generation != lease) {
        return error_e::not_leased;
      }
      impl.replacement_may_exist = true;
    }

    const auto wanted = mode;
    const auto created = impl.call<creation_e>([backend, alt, name, uid, wanted] {
                               return backend->add(alt, name, uid, wanted);
                             })
                           .value_or(creation_e::created_unverifiable);

    if (created == creation_e::not_created) {
      std::lock_guard lock {impl.mutex};
      if (impl.state == state_e::active && impl.generation == lease) {
        impl.replacement_may_exist = false;
      }
      return error_e::create_failed;
    }

    if (created != creation_e::created) {
      // Still owed: abandon_replacement() or release() removes it.
      return error_e::create_failed;
    }

    display_t display;
    const auto give_up_at = std::chrono::steady_clock::now() + impl.timeouts.readiness;
    for (;;) {
      if (!impl.still_leased(lease)) {
        return error_e::not_leased;
      }

      const auto device_id = impl.call<std::string>([backend, alt] {
        return backend->device_id(alt);
      });
      if (!device_id) {
        return error_e::busy;
      }
      if (!device_id->empty()) {
        display.device_id = *device_id;
        break;
      }

      if (std::chrono::steady_clock::now() >= give_up_at) {
        return error_e::not_ready;
      }

      std::this_thread::sleep_for(impl.timeouts.readiness_poll);
    }

    // The GDI name only exists once the display is on the desktop, which it
    // is already if Windows put it there by itself. Only used for the log.
    if (const auto resolved = impl.call<resolution_t>([backend, alt, wanted] {
          return backend->resolve(alt, wanted);
        });
        resolved && resolved->state == resolution_t::readiness_e::ready) {
      display.gdi_name = resolved->display.gdi_name;
    }

    {
      std::lock_guard lock {impl.mutex};
      if (impl.state != state_e::active || impl.generation != lease) {
        return error_e::not_leased;
      }
      impl.replacement = display;
    }

    BOOST_LOG(info) << "Virtual display to replace the streamed one is up"sv
                    << (display.gdi_name.empty() ? ""s : " as "s + display.gdi_name)
                    << " at "sv << wanted.width << 'x' << wanted.height
                    << " @ "sv << (wanted.refresh_rate_millihz / 1000.) << "Hz"sv;
    return display;
  }

  void manager_t::use_replacement(bool use) {
    std::lock_guard lock {m_impl->mutex};
    m_impl->replacement_in_use = use && m_impl->state == state_e::active && m_impl->replacement_may_exist &&
                                 !m_impl->replacement.device_id.empty();
  }

  bool manager_t::commit_replacement() {
    auto &impl = *m_impl;

    std::shared_ptr<backend_t> backend;
    uuid_util::uuid_t old_id {};
    std::uint64_t lease {};
    {
      std::lock_guard lock {impl.mutex};
      if (impl.state != state_e::active || !impl.replacement_may_exist || impl.replacement.device_id.empty()) {
        return false;
      }

      backend = impl.backend;
      old_id = impl.id;
      lease = impl.generation;

      // The replacement is the leased display from here on, whatever happens
      // to the old one, which moves to the other identity and stays owed
      // removal until it is gone.
      std::swap(impl.id, impl.alt_id);
      impl.display = std::exchange(impl.replacement, {});
      impl.replacement_in_use = false;
    }

    const auto removed = impl.call<bool>([backend, old_id] {
                               return backend->remove(old_id);
                             })
                           .value_or(false);

    if (removed) {
      std::lock_guard lock {impl.mutex};
      if (impl.state == state_e::active && impl.generation == lease) {
        impl.replacement_may_exist = false;
      }
    }
    return removed;
  }

  bool manager_t::abandon_replacement() {
    auto &impl = *m_impl;

    std::shared_ptr<backend_t> backend;
    uuid_util::uuid_t alt {};
    std::uint64_t lease {};
    {
      std::lock_guard lock {impl.mutex};
      impl.replacement_in_use = false;
      if (!impl.replacement_may_exist) {
        impl.replacement = {};
        return true;
      }
      if (impl.state != state_e::active) {
        // Nothing may be asked of the driver, and the obligation stays with
        // whatever finishes the lease.
        return false;
      }

      backend = impl.backend;
      alt = impl.alt_id;
      lease = impl.generation;
    }

    const auto removed = impl.call<bool>([backend, alt] {
                               return backend->remove(alt);
                             })
                           .value_or(false);

    if (removed) {
      std::lock_guard lock {impl.mutex};
      if (impl.state == state_e::active && impl.generation == lease) {
        impl.replacement_may_exist = false;
        impl.replacement = {};
      }
    }
    return removed;
  }

  /**
   * @brief One run of a restore, fixed at the moment it starts.
   *
   * Callbacks hold this by value rather than reading the transaction, so a
   * retry that was queued for one run can tell it no longer speaks for the
   * transaction and do nothing, instead of acting on whichever lease happens
   * to be current when it finally executes.
   */
  struct restore_run_t {
    std::uint64_t epoch {};  ///< Tells this run apart from the ones before and after it.
    std::uint64_t generation {};  ///< The lease this run is being done for.
    std::shared_ptr<std::promise<bool>> outcome;  ///< Where the answer is put.
    std::shared_future<bool> result;  ///< What callers wait on, including ones that joined.

    /// Whether someone has taken on finishing this run. Guarded by the
    /// transaction's mutex, so deciding to finish and being allowed to are
    /// the same step and only one caller can win.
    bool completion_claimed {false};

    /// Whether the answer has been handed over. Guarded by the
    /// transaction's mutex, like completion_claimed.
    bool settled {false};
  };

  /**
   * @brief The one restore in progress, and who it is for.
   */
  struct restore_transaction_t::impl_t {
    attempt_fn_t attempt;  ///< One try at restoring, from a caller holding no display stack locks.
    start_retries_fn_t start_retries;  ///< Starts retrying inside the display stack.
    release_fn_t release;  ///< Gives the display back, if the lease is still the expected one.

    mutable std::mutex mutex;  ///< Only ever held for bookkeeping, never across a callback.
    std::uint64_t next_epoch {1};  ///< Handed to the next run that starts.
    std::shared_ptr<restore_run_t> current;  ///< The run in progress, if there is one.

    /**
     * @brief Whether a run still speaks for the transaction.
     *
     * @param run The run to check.
     * @return True if it is the current one.
     */
    bool is_current(const std::shared_ptr<restore_run_t> &run) const {
      std::lock_guard lock {mutex};
      return current && current->epoch == run->epoch;
    }

    /**
     * @brief Take on finishing a run, if nobody else has.
     *
     * A retry that has just restored and a teardown that wants to give up
     * both arrive here, and they must not both go on to release the lease.
     * Checking that the run is current and claiming it happen together, so
     * exactly one of them wins.
     *
     * @param run The run to finish.
     * @return True if this caller is now the one that must release and settle.
     */
    bool claim_completion(const std::shared_ptr<restore_run_t> &run) {
      std::lock_guard lock {mutex};
      if (!current || current->epoch != run->epoch || run->completion_claimed) {
        return false;
      }
      run->completion_claimed = true;
      return true;
    }

    /**
     * @brief Finish a run: stop calling it current and hand over the answer.
     *
     * One step, in that order. A waiter woken by the answer may go straight
     * on to start the next restore, and it must not find the finished run
     * still sitting there to join.
     *
     * While a run is current a second caller joins its result rather than
     * starting a restore of its own, which is what keeps two of them from
     * running while one is mid-release.
     *
     * @param run The run that is finished.
     * @param restored Whether the configuration went back.
     */
    void publish(const std::shared_ptr<restore_run_t> &run, bool restored) {
      std::shared_ptr<std::promise<bool>> answer;
      {
        std::lock_guard lock {mutex};
        if (current && current->epoch == run->epoch) {
          current.reset();
        }
        if (run->settled) {
          return;
        }
        run->settled = true;
        answer = run->outcome;
      }

      // Outside the lock, and only once the run is no longer current, so
      // whoever this wakes cannot join a run that is over.
      answer->set_value(restored);
    }

    /**
     * @brief Release the lease and finish the run, whatever goes wrong.
     *
     * Only ever called by whoever won the completion claim. A release that
     * throws still has to finish the run: leaving it unsettled would park
     * every later caller on a promise nothing will ever fulfil.
     *
     * @param run The run being finished.
     * @param give_back Whether the display should be handed back.
     * @param restored What to tell whoever is waiting.
     */
    void complete(const std::shared_ptr<restore_run_t> &run, bool give_back, bool restored) {
      bool answer = restored;
      try {
        if (give_back && release) {
          release(run->generation);
        }
      } catch (const std::exception &ex) {
        BOOST_LOG(error) << "Giving the virtual display back threw: "sv << ex.what();
        answer = false;
      } catch (...) {
        BOOST_LOG(error) << "Giving the virtual display back threw"sv;
        answer = false;
      }

      publish(run, answer);
    }

    /**
     * @brief One attempt at restoring, treating a throw as a failed attempt.
     *
     * @param attempt What to try.
     * @return Whether the configuration went back.
     */
    static bool try_attempt(const attempt_fn_t &attempt) {
      if (!attempt) {
        return true;
      }

      try {
        return attempt().value_or(false);
      } catch (const std::exception &ex) {
        BOOST_LOG(error) << "Restoring the display configuration threw: "sv << ex.what();
      } catch (...) {
        BOOST_LOG(error) << "Restoring the display configuration threw"sv;
      }
      return false;
    }
  };

  restore_transaction_t::restore_transaction_t(attempt_fn_t attempt, start_retries_fn_t start_retries, release_fn_t release):
      m_impl {std::make_shared<impl_t>()} {
    m_impl->attempt = std::move(attempt);
    m_impl->start_retries = std::move(start_retries);
    m_impl->release = std::move(release);
  }

  restore_transaction_t::~restore_transaction_t() = default;

  bool restore_transaction_t::run(std::uint64_t generation, std::chrono::milliseconds timeout) {
    std::shared_ptr<restore_run_t> run;
    bool mine = false;

    {
      std::lock_guard lock {m_impl->mutex};
      if (m_impl->current) {
        // Joining rather than starting a second: the display stack holds one
        // retry at a time, and replacing it would leave nothing to give the
        // display back.
        run = m_impl->current;
      } else {
        run = std::make_shared<restore_run_t>();
        run->epoch = m_impl->next_epoch++;
        run->generation = generation;
        run->outcome = std::make_shared<std::promise<bool>>();
        run->result = run->outcome->get_future().share();
        m_impl->current = run;
        mine = true;
      }
    }

    if (mine) {
      // Outside the lock: everything below can call back into the display
      // stack, which takes locks of its own.
      auto impl = m_impl;
      const auto finish = [impl, run](const attempt_fn_t &attempt) {
        if (!impl->is_current(run)) {
          // Queued for a run that is over. Doing anything here would act on
          // a lease this callback knows nothing about.
          return true;
        }

        if (!impl_t::try_attempt(attempt)) {
          return false;
        }

        if (!impl->claim_completion(run)) {
          // Someone else is finishing this run. Two releases for one lease
          // is the thing the claim exists to prevent.
          return true;
        }

        impl->complete(run, true, true);
        return true;
      };

      if (!finish(m_impl->attempt ? m_impl->attempt : attempt_fn_t {[] {
            return true;
          }})) {
        // Not restored yet, so it keeps being retried inside the display
        // stack until it is.
        bool retrying = false;
        try {
          retrying = m_impl->start_retries && m_impl->start_retries([finish](attempt_fn_t attempt) {
            return finish(attempt);
          });
        } catch (const std::exception &ex) {
          // Failing to install the retry is failing to install it, however
          // it failed. What must not happen is the run being left unfinished
          // with callers waiting on a promise nothing will fulfil.
          BOOST_LOG(error) << "Arranging to retry the display restore threw: "sv << ex.what();
        } catch (...) {
          BOOST_LOG(error) << "Arranging to retry the display restore threw"sv;
        }

        if (!retrying) {
          BOOST_LOG(error) << "Nothing can retry restoring the display configuration"sv;
          if (m_impl->claim_completion(run)) {
            m_impl->complete(run, false, false);
          }
        }
      }
    }

    if (run->result.wait_for(timeout) != std::future_status::ready) {
      return false;
    }
    return run->result.get();
  }

  bool restore_transaction_t::pending() const {
    std::lock_guard lock {m_impl->mutex};
    return m_impl->current != nullptr;
  }

  void restore_transaction_t::abandon() {
    std::shared_ptr<restore_run_t> run;
    {
      std::lock_guard lock {m_impl->mutex};
      run = m_impl->current;
    }
    if (!run) {
      return;
    }

    if (!m_impl->claim_completion(run)) {
      // A retry got there first and is finishing the run. Waiting for it
      // means this returns with the transaction actually over, and without
      // a second release for the same lease.
      run->result.wait();
      return;
    }

    BOOST_LOG(warning) << "The display stack is going away while a restore is outstanding. "
                          "One last attempt, then the virtual display is given back either way, "
                          "since holding it would keep every later session out."sv;

    // Given back whatever the last attempt says, since holding the lease
    // for a transaction that can no longer finish would keep every later
    // session out.
    std::ignore = impl_t::try_attempt(m_impl->attempt);
    m_impl->complete(run, true, false);
  }

  manager_t &manager() {
    static manager_t instance {
      [] {
        return make_backend();
      },
      [](const std::string &gdi_name) {
        return display_device::device_id_for_display_name(gdi_name);
      }
    };
    return instance;
  }

  bool enabled() {
#ifndef _WIN32
    // There is no driver to create one, and refusing every session over a
    // setting that cannot apply here would be worse than ignoring it.
    return false;
#else
    // The configuration is what sets the new display's mode, so without it
    // the display would come up at whatever size the driver picked.
    return config::video.dd.virtual_display &&
           config::video.dd.configuration_option != config::video_t::dd_t::config_option_e::disabled;
#endif
  }

  mode_t requested_mode(const rtsp_stream::launch_session_t &session) {
    // The driver takes thousandths of a hertz, which is how it can offer
    // 59.94Hz alongside 60Hz. Moonlight asks in whole hertz.
    return {
      session.width > 0 ? session.width : 1920,
      session.height > 0 ? session.height : 1080,
      (session.fps > 0 ? session.fps : 60) * 1000
    };
  }

}  // namespace virtual_display
