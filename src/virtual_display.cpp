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
     * @brief Run something that may never return, and give up on it if it does not.
     *
     * A driver that stops answering blocks in the middle of a synchronous
     * device call, which cannot be cancelled. The thread running it is
     * therefore abandoned rather than waited on, and the backend it holds is
     * kept alive by the caller's shared reference so the abandoned thread
     * never touches freed memory. The caller sees a failure, which is what
     * fail-closed needs.
     *
     * @param work What to run. Must capture whatever keeps its subject alive.
     * @param deadline How long to wait for it.
     * @return The result, or nothing if the deadline passed first.
     */
    template<class R>
    std::optional<R> bounded(std::function<R()> work, std::chrono::milliseconds deadline) {
      auto task = std::make_shared<std::packaged_task<R()>>(std::move(work));
      auto result = task->get_future();

      std::thread {[task] {
        (*task)();
      }}.detach();

      if (result.wait_for(deadline) != std::future_status::ready) {
        return std::nullopt;
      }
      return result.get();
    }

    /**
     * @brief Keeps the driver aware that Sunshine is still here.
     *
     * The driver drops the displays of a client that goes quiet, so this runs
     * for as long as a lease is held. It is not joined on teardown: a stuck
     * driver call would make the join block, and the whole point of the
     * generation and the shared state is that an abandoned thread can be left
     * to notice on its own without affecting anything.
     */
    struct heartbeat_t {
      std::shared_ptr<backend_t> backend;
      std::chrono::milliseconds interval;
      std::chrono::milliseconds call_deadline;
      std::uint64_t generation;
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
     * @brief How many pings in a row may fail before the driver is given up on.
     *
     * One lost ping is not worth ending a session for. Several in a row means
     * the displays are about to be dropped underneath us.
     */
    constexpr unsigned max_ping_failures = 3;

    /**
     * @brief Ping until told to stop, or until the driver stops answering.
     *
     * @param state Shared heartbeat state, which also keeps the backend alive.
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

        auto backend = state->backend;
        const auto answered = bounded<bool>([backend] {
          return backend->ping();
        },
                                            state->call_deadline);

        if (answered.value_or(false)) {
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
    }
    return "unknown error";
  }

  /**
   * @brief Everything one manager owns, shared so the heartbeat can reach it.
   */
  struct manager_t::impl_t: std::enable_shared_from_this<manager_t::impl_t> {
    std::shared_ptr<backend_t> backend;
    device_id_lookup_t device_id_lookup;
    timeouts_t timeouts;

    mutable std::mutex mutex;
    bool has_lease {false};
    uuid_util::uuid_t id {};
    display_t display {};
    std::uint64_t generation {0};
    std::shared_ptr<heartbeat_t> heartbeat;

    /**
     * @brief Drop the lease because the driver stopped answering.
     *
     * The display is already gone, so nothing is removed. What matters is that
     * Sunshine stops naming it as the thing to capture.
     *
     * @param lost_generation Which lease the heartbeat belonged to.
     */
    void on_heartbeat_lost(std::uint64_t lost_generation) {
      std::lock_guard lock {mutex};
      if (!has_lease || generation != lost_generation) {
        // A heartbeat from a lease that has already ended.
        return;
      }

      has_lease = false;
      display = {};
      heartbeat.reset();
    }
  };

  manager_t::manager_t(std::unique_ptr<backend_t> backend, device_id_lookup_t device_id_lookup, timeouts_t timeouts):
      m_impl {std::make_shared<impl_t>()} {
    m_impl->backend = std::shared_ptr<backend_t> {backend.release()};
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

    {
      std::lock_guard lock {impl.mutex};
      if (impl.has_lease) {
        return error_e::already_leased;
      }
    }

    auto backend = impl.backend;
    if (!backend) {
      return error_e::driver_unavailable;
    }

    const auto deadline = impl.timeouts.call;
    const auto call = [&backend, deadline](auto fn) {
      return bounded<std::invoke_result_t<decltype(fn), backend_t &>>([backend, fn] {
        return fn(*backend);
      },
                                                                      deadline);
    };

    // Everything up to the first successful ping happens before anything is
    // created, so a driver that is missing or unwell costs nothing to unwind.
    if (!call([](backend_t &b) {
           return b.open();
         }).value_or(false)) {
      return error_e::driver_unavailable;
    }

    const auto fail = [&call](error_e error) {
      call([](backend_t &b) {
        b.close();
        return true;
      });
      return error;
    };

    if (!call([](backend_t &b) {
           return b.protocol_supported();
         }).value_or(false)) {
      return fail(error_e::protocol_mismatch);
    }

    const auto watchdog = call([](backend_t &b) {
      return b.watchdog_timeout();
    });
    if (!watchdog || !*watchdog || (*watchdog)->count() <= 0) {
      // Without a watchdog, a Sunshine that crashes leaves a display behind
      // with nothing able to remove it, so the lease is refused instead.
      return fail(error_e::watchdog_unavailable);
    }

    // Confirm the driver answers before asking it for anything, so a display
    // is never created for a driver that cannot be kept alive.
    if (!call([](backend_t &b) {
           return b.ping();
         }).value_or(false)) {
      return fail(error_e::heartbeat_failed);
    }

    if (!adapter_name.empty()) {
      call([&adapter_name](backend_t &b) {
        b.set_render_adapter(adapter_name);
        return true;
      });
    }

    // A fresh identifier every time. Reusing one, or guessing at the one a
    // previous run used, risks removing a display this process does not own.
    const auto id = uuid_util::uuid_t::generate();

    if (!call([&id, &client_name, &client_uid, &mode](backend_t &b) {
           return b.add(id, client_name, client_uid, mode);
         }).value_or(false)) {
      return fail(error_e::create_failed);
    }

    // From here the display exists, so every failure removes it.
    const auto fail_created = [&call, &fail, &id](error_e error) {
      call([&id](backend_t &b) {
        return b.remove(id);
      });
      return fail(error);
    };

    resolution_t::state_e last_state = resolution_t::state_e::not_ready;
    display_t display;

    const auto give_up_at = std::chrono::steady_clock::now() + impl.timeouts.readiness;
    for (;;) {
      const auto resolved = call([&id, &mode](backend_t &b) {
        return b.resolve(id, mode);
      });

      if (resolved) {
        last_state = resolved->state;
        if (last_state == resolution_t::state_e::ready) {
          // The display is up, but the device list Sunshine addresses
          // displays through can lag behind it, so a miss here is another
          // reason to keep waiting rather than a failure.
          display = resolved->display;
          display.device_id = impl.device_id_lookup ? impl.device_id_lookup(display.gdi_name) : std::string {};
          if (!display.device_id.empty()) {
            break;
          }
          last_state = resolution_t::state_e::not_ready;
        }
      }

      if (std::chrono::steady_clock::now() >= give_up_at) {
        return fail_created(
          last_state == resolution_t::state_e::mode_missing ? error_e::mode_unavailable : error_e::not_ready
        );
      }

      std::this_thread::sleep_for(impl.timeouts.readiness_poll);
    }

    auto heartbeat = std::make_shared<heartbeat_t>();
    heartbeat->backend = backend;
    // A third of the driver's patience, so two pings can be lost before it
    // decides Sunshine is gone.
    heartbeat->interval = std::chrono::duration_cast<std::chrono::milliseconds>(**watchdog) / 3;
    heartbeat->call_deadline = impl.timeouts.call;
    heartbeat->on_lost = [weak = m_impl->weak_from_this()](std::uint64_t generation) {
      if (auto impl = weak.lock()) {
        impl->on_heartbeat_lost(generation);
      }
    };

    {
      std::lock_guard lock {impl.mutex};
      impl.generation += 1;
      heartbeat->generation = impl.generation;
      impl.has_lease = true;
      impl.id = id;
      impl.display = display;
      impl.heartbeat = heartbeat;
    }

    std::thread {run_heartbeat, heartbeat}.detach();

    BOOST_LOG(info) << "Streaming virtual display "sv << display.gdi_name
                    << " at "sv << mode.width << 'x' << mode.height
                    << " @ "sv << (mode.refresh_rate_millihz / 1000.) << "Hz"sv;
    return display;
  }

  void manager_t::release() {
    auto &impl = *m_impl;

    std::shared_ptr<heartbeat_t> heartbeat;
    std::shared_ptr<backend_t> backend;
    uuid_util::uuid_t id {};
    bool had_lease = false;

    {
      std::lock_guard lock {impl.mutex};
      had_lease = impl.has_lease;
      heartbeat = std::exchange(impl.heartbeat, nullptr);
      backend = impl.backend;
      id = impl.id;
    }

    if (heartbeat) {
      heartbeat->request_stop();
    }

    if (!backend) {
      return;
    }

    // Removed while the lease still names it, so the display never stops
    // being accounted for while it still exists.
    if (had_lease) {
      const auto removed = bounded<bool>([backend, id] {
        return backend->remove(id);
      },
                                         impl.timeouts.call);
      if (!removed.value_or(false)) {
        BOOST_LOG(warning) << "Could not remove the virtual display. The driver drops it once Sunshine "
                              "stops answering its watchdog."sv;
      }
    }

    {
      std::lock_guard lock {impl.mutex};
      impl.has_lease = false;
      impl.display = {};
    }

    bounded<bool>([backend] {
      backend->close();
      return true;
    },
                  impl.timeouts.call);
  }

  bool manager_t::leased() const {
    std::lock_guard lock {m_impl->mutex};
    return m_impl->has_lease;
  }

  std::optional<std::string> manager_t::output_override() const {
    std::lock_guard lock {m_impl->mutex};
    if (!m_impl->has_lease) {
      return std::nullopt;
    }
    return m_impl->display.device_id;
  }

#ifndef _WIN32
  std::unique_ptr<backend_t> make_backend() {
    // No virtual display driver outside Windows. Every acquire then fails,
    // which is what the option's fail-closed contract requires.
    return nullptr;
  }
#endif

  manager_t &manager() {
    static manager_t instance {make_backend(), [](const std::string &gdi_name) {
                                 return display_device::device_id_for_display_name(gdi_name);
                               }};
    return instance;
  }

  bool enabled() {
#ifndef _WIN32
    // There is no driver to create one, and refusing every session because of
    // a setting that cannot apply here would be worse than ignoring it.
    return false;
#else
    // The configuration is what sets the new display's mode, so without it the
    // display would come up at whatever size the driver picked.
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
