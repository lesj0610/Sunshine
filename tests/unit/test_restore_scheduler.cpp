/**
 * @file tests/unit/test_restore_scheduler.cpp
 * @brief Drive the restore transaction through the real retry scheduler.
 *
 * The scheduler holds its own lock while it runs a scheduled callback, and
 * asking it for the interface again from in there takes that same lock a
 * second time. Nothing but the real scheduler catches that, so these tests
 * use it rather than a stand-in.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>

// lib includes
#include <display_device/retry_scheduler.h>

// test includes
#include "../tests_common.h"

// local includes
#include <src/virtual_display.h>

namespace {

  using namespace std::chrono_literals;

  /**
   * @brief Stands in for the settings manager, counting what it was asked.
   */
  struct fake_settings_t {
    std::mutex mutex;
    int reverts {};
    int fail_first {};  ///< How many attempts report failure before one works.

    /**
     * @brief Restore, failing the first few times if the test said so.
     */
    bool revert() {
      std::lock_guard lock {mutex};
      reverts += 1;
      return reverts > fail_first;
    }

    int attempts() {
      std::lock_guard lock {mutex};
      return reverts;
    }
  };

  using scheduler_t = display_device::RetryScheduler<fake_settings_t>;

  /**
   * @brief Build a transaction wired to a real scheduler over a fake manager.
   */
  std::unique_ptr<virtual_display::restore_transaction_t> make_transaction(
    const std::shared_ptr<scheduler_t> &scheduler,
    std::function<bool(std::uint64_t)> release,
    std::chrono::milliseconds retry_after = 300ms
  ) {
    return std::make_unique<virtual_display::restore_transaction_t>(
      [scheduler]() -> std::optional<bool> {
        // The first attempt runs from a caller holding none of the
        // scheduler's locks, so going through execute() is fine here.
        return scheduler->execute([](auto &settings) {
          return settings.revert();
        });
      },
      [scheduler, retry_after](std::function<bool(virtual_display::restore_transaction_t::attempt_fn_t)> finish) {
        scheduler->schedule([finish](auto &settings, auto &stop_token) {
          // Built from the interface the scheduler handed over. Calling
          // execute() here would take the lock it is already holding.
          const bool done {finish([&settings]() -> std::optional<bool> {
            return settings.revert();
          })};

          if (done) {
            stop_token.requestStop();
          }
        },
                            {.m_sleep_durations = {retry_after},
                             .m_execution = display_device::SchedulerOptions::Execution::ScheduledOnly});
        return true;
      },
      std::move(release)
    );
  }

}  // namespace

class RestoreSchedulerTest: public ::testing::Test {};

TEST_F(RestoreSchedulerTest, AScheduledRetryCompletesWithoutDeadlocking) {
  // The first attempt fails, so the rest happen inside the scheduler thread
  // while it holds its own lock. If the retry reached back into the
  // scheduler this would never return.
  auto settings = std::make_unique<fake_settings_t>();
  settings->fail_first = 1;
  auto *settings_raw = settings.get();
  auto scheduler = std::make_shared<scheduler_t>(std::move(settings));

  std::promise<std::uint64_t> released;
  auto handed_back = released.get_future();
  std::atomic<int> releases {0};

  auto transaction = make_transaction(scheduler, [&released, &releases](std::uint64_t generation) {
    if (releases.fetch_add(1) == 0) {
      released.set_value(generation);
    }
    return true;
  });

  // The retry is 300ms away, so this returns before it has happened.
  EXPECT_FALSE(transaction->run(21, 50ms));
  EXPECT_EQ(releases.load(), 0);

  // And then it happens, inside the scheduler thread, holding the scheduler's
  // own lock. A retry that reached back into the scheduler would stop here.
  ASSERT_EQ(handed_back.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(handed_back.get(), 21u);
  EXPECT_EQ(releases.load(), 1);
  EXPECT_GE(settings_raw->attempts(), 2);

  // And it stops there rather than going round again.
  // isScheduled() is deliberately not used: it reads the scheduler's retry
  // function without taking the scheduler's lock, which races with the
  // scheduler thread clearing it. Sunshine never calls it.
  const auto attempts_at_success = settings_raw->attempts();
  std::this_thread::sleep_for(700ms);
  EXPECT_EQ(settings_raw->attempts(), attempts_at_success);
  EXPECT_EQ(releases.load(), 1);
}

TEST_F(RestoreSchedulerTest, ARestoreThatWorksStraightAwayNeverSchedules) {
  auto settings = std::make_unique<fake_settings_t>();
  auto *settings_raw = settings.get();
  auto scheduler = std::make_shared<scheduler_t>(std::move(settings));

  std::atomic<int> releases {0};
  auto transaction = make_transaction(scheduler, [&releases](std::uint64_t) {
    releases += 1;
    return true;
  });

  EXPECT_TRUE(transaction->run(1, 5s));
  EXPECT_EQ(releases.load(), 1);

  // One attempt, made by the caller. Nothing was left for the scheduler.
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(settings_raw->attempts(), 1);
}

TEST_F(RestoreSchedulerTest, AbandoningWhileTheSchedulerRetriesDoesNotStrandTheLease) {
  auto settings = std::make_unique<fake_settings_t>();
  settings->fail_first = 1000;  // never succeeds on its own
  auto scheduler = std::make_shared<scheduler_t>(std::move(settings));

  std::atomic<int> releases {0};
  std::atomic<std::uint64_t> last {0};
  auto transaction = make_transaction(scheduler, [&releases, &last](std::uint64_t generation) {
    releases += 1;
    last = generation;
    return true;
  });

  ASSERT_FALSE(transaction->run(33, 50ms));
  ASSERT_TRUE(transaction->pending());

  transaction->abandon();

  EXPECT_FALSE(transaction->pending());
  EXPECT_EQ(releases.load(), 1);
  EXPECT_EQ(last.load(), 33u);

  // Whatever the scheduler is still doing must not give anything else back.
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(releases.load(), 1);

  scheduler->stop();
}

namespace {

  /**
   * @brief Lets a test hold a release open and let it go on demand.
   */
  struct release_gate_t {
    std::mutex mutex;
    std::condition_variable cv;
    bool open {false};
    int arrived {};

    void wait() {
      std::unique_lock lock {mutex};
      arrived += 1;
      cv.notify_all();
      cv.wait(lock, [this] {
        return open;
      });
    }

    void await_arrival() {
      std::unique_lock lock {mutex};
      cv.wait(lock, [this] {
        return arrived > 0;
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

}  // namespace

TEST_F(RestoreSchedulerTest, ASecondRunJoinsOneThatIsStillReleasing) {
  // While the release is in progress the run is still current, so a second
  // caller waits on it rather than starting a restore of its own.
  auto settings = std::make_unique<fake_settings_t>();
  auto *settings_raw = settings.get();
  auto scheduler = std::make_shared<scheduler_t>(std::move(settings));

  auto gate = std::make_shared<release_gate_t>();
  std::atomic<int> releases {0};
  auto transaction = make_transaction(scheduler, [gate, &releases](std::uint64_t) {
    releases += 1;
    gate->wait();
    return true;
  });

  auto first = std::async(std::launch::async, [&transaction] {
    return transaction->run(7, 10s);
  });
  gate->await_arrival();

  // Called from here rather than another thread, so it definitely happens
  // while the release is still in progress. It joins the run that is already
  // going, which cannot settle until the gate opens, so it times out rather
  // than starting a restore of its own.
  EXPECT_FALSE(transaction->run(7, 50ms));
  EXPECT_EQ(releases.load(), 1);
  EXPECT_EQ(settings_raw->attempts(), 1);

  gate->release();

  EXPECT_TRUE(first.get());
  EXPECT_EQ(releases.load(), 1);
  EXPECT_EQ(settings_raw->attempts(), 1);

  // And once it is over, the next caller gets a run of its own.
  EXPECT_FALSE(transaction->pending());
}

TEST_F(RestoreSchedulerTest, ASucceedingRetryAndAbandonFinishTheRunOnce) {
  // The retry succeeds while a teardown is trying to give up. Exactly one of
  // them releases the lease and settles the run.
  auto settings = std::make_unique<fake_settings_t>();
  settings->fail_first = 1;
  auto scheduler = std::make_shared<scheduler_t>(std::move(settings));

  auto gate = std::make_shared<release_gate_t>();
  std::atomic<int> releases {0};
  auto transaction = make_transaction(
    scheduler,
    [gate, &releases](std::uint64_t) {
      releases += 1;
      gate->wait();
      return true;
    },
    50ms
  );

  ASSERT_FALSE(transaction->run(12, 20ms));

  // The retry gets there first and is held inside the release.
  gate->await_arrival();

  auto abandoning = std::async(std::launch::async, [&transaction] {
    transaction->abandon();
  });
  std::this_thread::sleep_for(20ms);
  gate->release();

  ASSERT_EQ(abandoning.wait_for(10s), std::future_status::ready);
  abandoning.get();

  EXPECT_EQ(releases.load(), 1);
  EXPECT_FALSE(transaction->pending());

  scheduler->stop();
}
