/**
 * @file tests/unit/test_virtual_display.cpp
 * @brief Test the virtual display lease against a driver that does as it is told.
 *
 * The lease is the part that has to be right whether or not a driver is
 * present: it decides when a session may proceed, and it is the only thing
 * that removes a display once one exists. Every case here is a way for a real
 * driver to misbehave.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// test includes
#include "../tests_common.h"

// local includes
#include <src/rtsp.h>
#include <src/virtual_display.h>

namespace {

  using namespace std::chrono_literals;

  constexpr virtual_display::mode_t k_mode {1920, 1080, 60000};

  using creation_e_alias = virtual_display::creation_e;

  /**
   * @brief Lets a test hold a backend call open and let it go on demand.
   */
  struct gate_t {
    std::mutex mutex;
    std::condition_variable cv;
    bool open {false};
    int waiting {0};

    /// Block until the test opens the gate.
    void wait() {
      std::unique_lock lock {mutex};
      waiting += 1;
      cv.notify_all();
      cv.wait(lock, [this] {
        return open;
      });
    }

    /// Wait until someone is held at the gate.
    void await_arrival() {
      std::unique_lock lock {mutex};
      cv.wait(lock, [this] {
        return waiting > 0;
      });
    }

    void release() {
      {
        std::lock_guard lock {mutex};
        open = true;
      }
      cv.notify_all();
    }
  };

  /**
   * @brief A driver that behaves exactly as a test tells it to.
   */
  struct fake_backend_t: virtual_display::backend_t {
    /**
     * @brief What the fake is told to do.
     */
    struct script_t {
      bool open_succeeds {true};
      bool protocol_supported {true};
      std::optional<std::chrono::seconds> watchdog {std::chrono::seconds {9}};
      bool ping_succeeds {true};
      creation_e_alias add_result {virtual_display::creation_e::created};
      bool remove_succeeds {true};

      /// How many resolve() calls report "not up yet" before the real answer.
      int not_ready_calls {0};
      /// What resolve() reports once it stops saying "not up yet".
      virtual_display::resolution_t::readiness_e eventual_state {virtual_display::resolution_t::readiness_e::ready};
      std::string gdi_name {"\\\\.\\DISPLAY7"};
    };

    /**
     * @brief What the fake was asked to do.
     */
    struct log_t {
      std::mutex mutex;  ///< Two acquires can be running at once.
      int opens {};
      int closes {};
      int adds {};
      int removes {};
      int pings {};
      std::vector<std::string> render_adapters;
      std::set<std::string> added_ids;
      std::set<std::string> removed_ids;
      virtual_display::mode_t last_mode {};
      bool fail_pings {};  ///< Set mid-test to make the driver go quiet.
    };

    script_t script;
    std::shared_ptr<gate_t> add_gate;  ///< When set, add() waits on it.

    // The log outlives the backend, so a test can still read it after the
    // manager that owns the backend has been destroyed.
    std::shared_ptr<log_t> log {std::make_shared<log_t>()};

    bool open() override {
      log->opens += 1;
      return script.open_succeeds;
    }

    void close() override {
      log->closes += 1;
    }

    bool protocol_supported() override {
      return script.protocol_supported;
    }

    std::optional<std::chrono::seconds> watchdog_timeout() override {
      return script.watchdog;
    }

    bool ping() override {
      std::lock_guard lock {log->mutex};
      log->pings += 1;
      return script.ping_succeeds && !log->fail_pings;
    }

    void set_render_adapter(const std::string &adapter_name) override {
      log->render_adapters.push_back(adapter_name);
    }

    virtual_display::creation_e add(const uuid_util::uuid_t &id, const std::string &, const std::string &, const virtual_display::mode_t &mode) override {
      {
        std::lock_guard lock {log->mutex};
        log->adds += 1;
        log->last_mode = mode;
      }
      if (add_gate) {
        add_gate->wait();
      }
      if (script.add_result == virtual_display::creation_e::not_created) {
        return script.add_result;
      }
      std::lock_guard lock {log->mutex};
      log->added_ids.insert(id.string());
      return script.add_result;
    }

    bool remove(const uuid_util::uuid_t &id) override {
      std::lock_guard lock {log->mutex};
      log->removes += 1;
      log->removed_ids.insert(id.string());
      return script.remove_succeeds;
    }

    virtual_display::resolution_t resolve(const uuid_util::uuid_t &id, const virtual_display::mode_t &) override {
      if (!log->added_ids.contains(id.string())) {
        return {};
      }
      if (m_resolves++ < script.not_ready_calls) {
        return {};
      }
      if (script.eventual_state != virtual_display::resolution_t::readiness_e::ready) {
        return {script.eventual_state, {}};
      }
      return {virtual_display::resolution_t::readiness_e::ready, {{}, script.gdi_name}};
    }

  private:
    int m_resolves {0};
  };

  /**
   * @brief Timeouts short enough that a failing test does not stall the suite.
   */
  virtual_display::timeouts_t quick_timeouts() {
    // Long enough that a held-open call is a deliberate test and not a busy
    // machine, short enough that a failing test does not stall the suite.
    return {.readiness = 300ms, .readiness_poll = 10ms, .call = 1500ms};
  }

  /**
   * @brief Build a manager over a fake, keeping a pointer to the fake.
   *
   * @param script How the fake should behave.
   * @param lookup Device id translation, defaulting to one that always works.
   * @return The manager and the fake it is driving.
   */
  /**
   * @brief What a test drives a manager with.
   */
  struct rig_t {
    std::shared_ptr<fake_backend_t::log_t> log;
    std::shared_ptr<gate_t> add_gate;
    std::unique_ptr<virtual_display::manager_t> manager;
  };

  /**
   * @brief Build a manager over fakes that all share one log.
   *
   * The manager makes a backend when it needs one and throws it away when it
   * stops answering, so the log lives outside them and outlives them all.
   *
   * @param script How the fakes should behave.
   * @param add_gate When set, every add() waits on it.
   * @param lookup Device id translation, defaulting to one that always works.
   * @return The manager, its log, and the gate.
   */
  rig_t make_rig(
    fake_backend_t::script_t script,
    std::shared_ptr<gate_t> add_gate = nullptr,
    virtual_display::device_id_lookup_t lookup = [](const std::string &name) {
      return name + "-id";
    }
  ) {
    auto log = std::make_shared<fake_backend_t::log_t>();
    auto factory = [script, log, add_gate]() -> std::unique_ptr<virtual_display::backend_t> {
      auto backend = std::make_unique<fake_backend_t>();
      backend->script = script;
      backend->log = log;
      backend->add_gate = add_gate;
      return backend;
    };

    return {
      log,
      add_gate,
      std::make_unique<virtual_display::manager_t>(std::move(factory), std::move(lookup), quick_timeouts())
    };
  }

  /**
   * @brief The error an acquire reported, for tests that expect one.
   *
   * @param result What acquire returned.
   * @return The error.
   */
  virtual_display::error_e error_of(const std::variant<virtual_display::display_t, virtual_display::error_e> &result) {
    return std::get<virtual_display::error_e>(result);
  }

}  // namespace

class VirtualDisplayTest: public ::testing::Test {};

TEST_F(VirtualDisplayTest, AcquireCreatesADisplayAndTakesTheLease) {
  auto [log, gate, manager] = make_rig({});

  const auto result = manager->acquire("Client", "uid", k_mode, "");

  const auto &display = std::get<virtual_display::display_t>(result);
  EXPECT_EQ(display.gdi_name, "\\\\.\\DISPLAY7");
  EXPECT_EQ(display.device_id, "\\\\.\\DISPLAY7-id");
  EXPECT_TRUE(manager->leased());
  EXPECT_EQ(manager->output_override(), display.device_id);
  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->adds, 1);
}

TEST_F(VirtualDisplayTest, RefreshRateReachesTheDriverInThousandthsOfAHertz) {
  // The driver's mode table is in thousandths of a hertz, which is how it can
  // offer 59.94Hz next to 60Hz. Sending whole hertz would ask for 0.06Hz.
  for (const auto &[fps, expected] : {std::pair {60, 60000}, std::pair {120, 120000}, std::pair {30, 30000}}) {
    auto [log, gate, manager] = make_rig({});

    const auto result = manager->acquire("Client", "uid", {1920, 1080, fps * 1000}, "");

    ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(result));
    EXPECT_EQ(log->last_mode.refresh_rate_millihz, expected);
  }
}

TEST_F(VirtualDisplayTest, RequestedModeIsTakenFromTheSessionInDriverUnits) {
  rtsp_stream::launch_session_t session {};
  session.width = 2560;
  session.height = 1440;
  session.fps = 120;

  const auto mode = virtual_display::requested_mode(session);

  EXPECT_EQ(mode.width, 2560);
  EXPECT_EQ(mode.height, 1440);
  EXPECT_EQ(mode.refresh_rate_millihz, 120000);
}

TEST_F(VirtualDisplayTest, NoBackendMeansNoLease) {
  virtual_display::manager_t manager {nullptr, [](const std::string &n) {
                                        return n;
                                      },
                                      quick_timeouts()};

  EXPECT_EQ(error_of(manager.acquire("Client", "uid", k_mode, "")), virtual_display::error_e::driver_unavailable);
  EXPECT_FALSE(manager.leased());
  EXPECT_FALSE(manager.output_override());
}

TEST_F(VirtualDisplayTest, ADriverThatWillNotOpenIsNotUsed) {
  auto [log, gate, manager] = make_rig({.open_succeeds = false});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::driver_unavailable);
  EXPECT_EQ(log->adds, 0);
  EXPECT_FALSE(manager->leased());
}

TEST_F(VirtualDisplayTest, AnIncompatibleProtocolIsRefusedBeforeAnythingIsCreated) {
  auto [log, gate, manager] = make_rig({.protocol_supported = false});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::protocol_mismatch);
  EXPECT_EQ(log->adds, 0);
  EXPECT_EQ(log->closes, 1);
}

TEST_F(VirtualDisplayTest, ADriverWithNoWatchdogIsRefused) {
  // Without a watchdog a display would outlive a Sunshine that crashed, with
  // nothing left able to remove it.
  auto [log, gate, manager] = make_rig({.watchdog = std::chrono::seconds {0}});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::watchdog_unavailable);
  EXPECT_EQ(log->adds, 0);
}

TEST_F(VirtualDisplayTest, AnUnreportedWatchdogIsRefused) {
  auto [log, gate, manager] = make_rig({.watchdog = std::nullopt});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::watchdog_unavailable);
  EXPECT_EQ(log->adds, 0);
}

TEST_F(VirtualDisplayTest, TheDriverIsPingedBeforeADisplayIsCreated) {
  auto [log, gate, manager] = make_rig({.ping_succeeds = false});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::heartbeat_failed);
  EXPECT_GE(log->pings, 1);
  EXPECT_EQ(log->adds, 0);
}

TEST_F(VirtualDisplayTest, ARefusedCreateLeavesNoLease) {
  auto [log, gate, manager] = make_rig({.add_result = virtual_display::creation_e::not_created});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::create_failed);
  EXPECT_FALSE(manager->leased());
  EXPECT_EQ(log->closes, 1);
}

TEST_F(VirtualDisplayTest, ADisplayThatNeverComesUpIsRemovedAgain) {
  auto [log, gate, manager] = make_rig({.not_ready_calls = 1000});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::not_ready);
  EXPECT_EQ(log->removes, 1);
  EXPECT_FALSE(manager->leased());
}

TEST_F(VirtualDisplayTest, ADisplayWithoutTheRequestedModeIsRemovedAgain) {
  // A display that came up at some other size would stream at that size.
  auto [log, gate, manager] = make_rig({.eventual_state = virtual_display::resolution_t::readiness_e::mode_missing});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::mode_unavailable);
  EXPECT_EQ(log->removes, 1);
  EXPECT_FALSE(manager->leased());
}

TEST_F(VirtualDisplayTest, ADisplayMissingFromTheDeviceListIsRemovedAgain) {
  // Without an id, nothing can be pointed at the display, so a session would
  // capture some other screen.
  auto [log, gate, manager] = make_rig({}, nullptr, [](const std::string &) {
    return std::string {};
  });

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::not_ready);
  EXPECT_EQ(log->removes, 1);
  EXPECT_FALSE(manager->leased());
}

TEST_F(VirtualDisplayTest, ADisplayThatTakesAMomentIsWaitedFor) {
  auto [log, gate, manager] = make_rig({.not_ready_calls = 3});

  const auto result = manager->acquire("Client", "uid", k_mode, "");

  EXPECT_TRUE(std::holds_alternative<virtual_display::display_t>(result));
  EXPECT_EQ(log->removes, 0);
}

TEST_F(VirtualDisplayTest, ASecondSessionIsRefusedRatherThanSharingTheFirst) {
  auto [log, gate, manager] = make_rig({});
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("First", "uid-1", k_mode, "")));

  const auto second = manager->acquire("Second", "uid-2", {1280, 720, 60000}, "");

  EXPECT_EQ(error_of(second), virtual_display::error_e::already_leased);
  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->adds, 1);
}

TEST_F(VirtualDisplayTest, ReleaseRemovesTheDisplayItCreated) {
  auto [log, gate, manager] = make_rig({});
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  manager->release();

  EXPECT_FALSE(manager->leased());
  EXPECT_FALSE(manager->output_override());
  EXPECT_EQ(log->removes, 1);
  EXPECT_EQ(log->added_ids, log->removed_ids);
  EXPECT_EQ(log->closes, 1);
}

TEST_F(VirtualDisplayTest, ReleaseWithoutALeaseTouchesNothing) {
  auto [log, gate, manager] = make_rig({});

  manager->release();

  EXPECT_EQ(log->removes, 0);
}

TEST_F(VirtualDisplayTest, AFailedRemoveStillGivesUpTheLease) {
  // The driver's watchdog will drop the display once Sunshine stops answering,
  // so holding the lease open would only stop the next session from starting.
  auto [log, gate, manager] = make_rig({.remove_succeeds = false});
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  manager->release();

  EXPECT_FALSE(manager->leased());
  EXPECT_EQ(log->removes, 1);
}

TEST_F(VirtualDisplayTest, EachLeaseUsesAnIdentifierOfItsOwn) {
  // Reusing an identifier, or guessing the one a previous run used, risks
  // removing a display this process does not own.
  auto [log, gate, manager] = make_rig({});

  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));
  manager->release();
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));
  manager->release();

  EXPECT_EQ(log->adds, 2);
  EXPECT_EQ(log->added_ids.size(), 2u);
}

TEST_F(VirtualDisplayTest, NoDisplayIsRemovedBeforeOneIsCreated) {
  // A lease never removes a display it did not create, so a first acquire
  // issues no removals at all.
  auto [log, gate, manager] = make_rig({});

  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  EXPECT_EQ(log->removes, 0);
}

TEST_F(VirtualDisplayTest, TheConfiguredAdapterIsPassedOn) {
  auto [log, gate, manager] = make_rig({});

  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "Some GPU")));

  EXPECT_EQ(log->render_adapters, std::vector<std::string> {"Some GPU"});
}

TEST_F(VirtualDisplayTest, NoAdapterIsRequestedWhenNoneIsConfigured) {
  auto [log, gate, manager] = make_rig({});

  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  EXPECT_TRUE(log->render_adapters.empty());
}

TEST_F(VirtualDisplayTest, DestroyingTheManagerReleasesTheDisplay) {
  auto [log, gate, manager] = make_rig({});
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  manager.reset();

  EXPECT_EQ(log->removes, 1);
}

TEST_F(VirtualDisplayTest, EveryErrorHasSomethingToSay) {
  for (const auto error : {
         virtual_display::error_e::driver_unavailable,
         virtual_display::error_e::protocol_mismatch,
         virtual_display::error_e::watchdog_unavailable,
         virtual_display::error_e::heartbeat_failed,
         virtual_display::error_e::create_failed,
         virtual_display::error_e::not_ready,
         virtual_display::error_e::mode_unavailable,
         virtual_display::error_e::already_leased
       }) {
    EXPECT_FALSE(virtual_display::to_string(error).empty());
  }
}

TEST_F(VirtualDisplayTest, TwoAcquiresArrivingTogetherCreateOneDisplay) {
  // The lease is reserved before any work starts, so the second request finds
  // it taken even though the first has not finished.
  auto gate = std::make_shared<gate_t>();
  auto [log, unused, manager] = make_rig({}, gate);

  auto first = std::async(std::launch::async, [&manager] {
    return manager->acquire("First", "uid-1", k_mode, "");
  });

  gate->await_arrival();
  const auto second = manager->acquire("Second", "uid-2", k_mode, "");
  gate->release();

  EXPECT_TRUE(std::holds_alternative<virtual_display::display_t>(first.get()));
  EXPECT_EQ(error_of(second), virtual_display::error_e::already_leased);

  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->adds, 1);
}

TEST_F(VirtualDisplayTest, ACallThatDoesNotComeBackPoisonsTheDriver) {
  // Nothing may be asked of a driver that still has an earlier call, because
  // the answer to that call could arrive after whatever came next.
  auto gate = std::make_shared<gate_t>();
  auto [log, unused, manager] = make_rig({}, gate);

  auto stuck = std::async(std::launch::async, [&manager] {
    return manager->acquire("Client", "uid", k_mode, "");
  });
  gate->await_arrival();

  // The call is still in the fake, so the acquire above times out on it.
  EXPECT_EQ(error_of(stuck.get()), virtual_display::error_e::create_failed);
  EXPECT_EQ(manager->state(), virtual_display::state_e::poisoned);

  // And a new session cannot start while it is still in there.
  EXPECT_EQ(error_of(manager->acquire("Next", "uid-2", k_mode, "")), virtual_display::error_e::busy);

  gate->release();
}

TEST_F(VirtualDisplayTest, NothingIsAskedOfTheDriverWhilePoisoned) {
  auto gate = std::make_shared<gate_t>();
  auto [log, unused, manager] = make_rig({}, gate);

  auto stuck = std::async(std::launch::async, [&manager] {
    return manager->acquire("Client", "uid", k_mode, "");
  });
  gate->await_arrival();
  std::ignore = stuck.get();

  int adds_when_poisoned = 0;
  {
    std::lock_guard lock {log->mutex};
    adds_when_poisoned = log->adds;
  }

  std::ignore = manager->acquire("Next", "uid-2", k_mode, "");
  manager->release();

  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->adds, adds_when_poisoned);
  EXPECT_EQ(log->removes, 0);

  gate->release();
}

TEST_F(VirtualDisplayTest, ADriverThatAnswersLateCanBeUsedAgain) {
  auto gate = std::make_shared<gate_t>();
  auto [log, unused, manager] = make_rig({}, gate);

  auto stuck = std::async(std::launch::async, [&manager] {
    return manager->acquire("Client", "uid", k_mode, "");
  });
  gate->await_arrival();
  std::ignore = stuck.get();
  ASSERT_EQ(manager->state(), virtual_display::state_e::poisoned);

  // The overdue call finally comes back.
  gate->release();
  for (int i = 0; i < 100 && manager->state() == virtual_display::state_e::poisoned; ++i) {
    std::ignore = manager->acquire("Probe", "uid-probe", k_mode, "");
    std::this_thread::sleep_for(10ms);
  }

  EXPECT_NE(manager->state(), virtual_display::state_e::poisoned);
}

TEST_F(VirtualDisplayTest, ADisplayTheDriverCannotDescribeIsStillRemoved) {
  // The driver took the request, so something may exist under an identifier
  // we know. Forgetting it would leave a display nothing can reach.
  auto [log, gate, manager] = make_rig({.add_result = virtual_display::creation_e::created_unverifiable});

  EXPECT_EQ(error_of(manager->acquire("Client", "uid", k_mode, "")), virtual_display::error_e::create_failed);

  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->removes, 1);
}

TEST_F(VirtualDisplayTest, LosingTheHeartbeatAsksTheOwnerToCleanUp) {
  auto [log, gate, manager] = make_rig({.watchdog = std::chrono::seconds {1}, .ping_succeeds = true});
  ASSERT_TRUE(std::holds_alternative<virtual_display::display_t>(manager->acquire("Client", "uid", k_mode, "")));

  std::promise<void> called;
  auto handled = called.get_future();
  manager->set_fault_handler([&manager, &called] {
    // What the real owner does: stop the stream, restore the display, and
    // only then give up the lease.
    manager->release();
    called.set_value();
  });

  // Every ping from here fails.
  {
    std::lock_guard lock {log->mutex};
    log->fail_pings = true;
  }

  ASSERT_EQ(handled.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(manager->leased());
  EXPECT_FALSE(manager->output_override());

  std::lock_guard lock {log->mutex};
  EXPECT_EQ(log->removes, 1);
}
