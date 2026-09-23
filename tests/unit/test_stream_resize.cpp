/**
 * @file tests/unit/test_stream_resize.cpp
 * @brief Test the resize transaction against steps that do as they are told.
 *
 * What a resize does to the host is behind an interface, so every way a step
 * can fail is a script here: the order steps are undone in, what the client
 * is told, the order it is told in, and when the session has to end.
 */
// standard includes
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// test includes
#include "../tests_common.h"

// local includes
#include <src/stream_resize.h>

// lib includes
#include <moonlight-common-c/src/StreamResize.h>

namespace {

  using namespace std::chrono_literals;
  using stream_resize::status_e;
  using stream_resize::stream_mode_t;

  constexpr stream_mode_t k_start {1920, 1080, 60};
  constexpr stream_mode_t k_wide {2560, 1080, 60};
  constexpr stream_mode_t k_tall {1280, 1440, 60};
  constexpr stream_mode_t k_small {1280, 720, 60};

  /**
   * @brief Steps that report what they were asked and do what the script says.
   */
  struct fake_steps_t: stream_resize::steps_t {
    /**
     * @brief Shared between the test and the steps, which the coordinator owns.
     */
    struct state_t {
      std::mutex mutex;
      std::condition_variable cv;

      std::map<std::string, bool> outcome;  ///< Step name to what it returns; missing means true.
      std::string hold;  ///< A step that waits until released or interrupted.
      bool held {false};  ///< Whether a step is waiting at the hold.
      bool released {false};
      bool interrupted {false};
      bool ignore_interrupt {false};  ///< The held step finishes normally even when interrupted.

      std::vector<std::string> calls;  ///< Step names in the order they were called.
      std::vector<std::uint64_t> generations;  ///< The generation each call was given.
      std::vector<stream_mode_t> modes;  ///< Modes passed to the steps that take one.
      int end_session_calls {0};
    };

    std::shared_ptr<state_t> state;

    bool step(const std::string &name, std::uint64_t generation) {
      std::unique_lock lock {state->mutex};
      state->calls.push_back(name);
      state->generations.push_back(generation);

      if (state->hold == name) {
        state->held = true;
        state->cv.notify_all();
        state->cv.wait(lock, [this] {
          return state->released || (state->interrupted && !state->ignore_interrupt);
        });
        state->held = false;
        if (state->interrupted && !state->ignore_interrupt) {
          return false;
        }
      }

      const auto outcome = state->outcome.find(name);
      return outcome == state->outcome.end() || outcome->second;
    }

    bool step(const std::string &name, std::uint64_t generation, const stream_mode_t &mode) {
      {
        std::lock_guard lock {state->mutex};
        state->modes.push_back(mode);
      }
      return step(name, generation);
    }

    bool create_display(std::uint64_t generation, const stream_mode_t &mode) override {
      return step("create_display", generation, mode);
    }

    bool switch_topology(std::uint64_t generation) override {
      return step("switch_topology", generation);
    }

    bool switch_video(std::uint64_t generation, const stream_mode_t &mode) override {
      return step("switch_video", generation, mode);
    }

    bool remove_old_display(std::uint64_t generation) override {
      return step("remove_old_display", generation);
    }

    bool restore_topology(std::uint64_t generation) override {
      return step("restore_topology", generation);
    }

    bool restore_video(std::uint64_t generation, const stream_mode_t &mode) override {
      return step("restore_video", generation, mode);
    }

    bool remove_new_display(std::uint64_t generation) override {
      return step("remove_new_display", generation);
    }

    void end_session() override {
      std::lock_guard lock {state->mutex};
      state->end_session_calls += 1;
      state->cv.notify_all();
    }

    void interrupt() override {
      std::lock_guard lock {state->mutex};
      state->interrupted = true;
      state->cv.notify_all();
    }
  };

  /**
   * @brief Answers the client would have been sent.
   */
  struct sent_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<stream_resize::result_t> results;

    /// Wait until at least count answers have been sent.
    bool await(std::size_t count) {
      std::unique_lock lock {mutex};
      return cv.wait_for(lock, 5s, [&] {
        return results.size() >= count;
      });
    }

    std::vector<stream_resize::result_t> snapshot() {
      std::lock_guard lock {mutex};
      return results;
    }
  };

  /**
   * @brief A coordinator over fake steps, with what it sent.
   */
  struct rig_t {
    std::shared_ptr<fake_steps_t::state_t> steps;
    std::shared_ptr<sent_t> sent;
    std::unique_ptr<stream_resize::coordinator_t> coordinator;

    /// Wait until the step being held is waiting at the hold.
    bool await_hold() {
      std::unique_lock lock {steps->mutex};
      return steps->cv.wait_for(lock, 5s, [this] {
        return steps->held;
      });
    }

    void release() {
      std::lock_guard lock {steps->mutex};
      steps->released = true;
      steps->cv.notify_all();
    }

    std::vector<std::string> calls() {
      std::lock_guard lock {steps->mutex};
      return steps->calls;
    }

    int end_session_calls() {
      std::lock_guard lock {steps->mutex};
      return steps->end_session_calls;
    }
  };

  rig_t make_rig(std::map<std::string, bool> outcome = {}, std::string hold = {}, stream_resize::options_t options = {}) {
    auto steps = std::make_shared<fake_steps_t::state_t>();
    steps->outcome = std::move(outcome);
    steps->hold = std::move(hold);

    auto fake = std::make_unique<fake_steps_t>();
    fake->state = steps;

    auto sent = std::make_shared<sent_t>();
    auto send = [sent](const stream_resize::result_t &result) {
      std::lock_guard lock {sent->mutex};
      sent->results.push_back(result);
      sent->cv.notify_all();
    };

    return {steps, sent, std::make_unique<stream_resize::coordinator_t>(std::move(fake), k_start, send, options)};
  }

  const std::vector<std::string> k_forward {"create_display", "switch_topology", "switch_video", "remove_old_display"};

}  // namespace

class StreamResizeTest: public ::testing::Test {};

TEST_F(StreamResizeTest, StatusValuesAreTheOnesOnTheWire) {
  EXPECT_EQ(static_cast<int>(status_e::ok), LI_STREAM_RESIZE_OK);
  EXPECT_EQ(static_cast<int>(status_e::superseded), LI_STREAM_RESIZE_SUPERSEDED);
  EXPECT_EQ(static_cast<int>(status_e::rejected_invalid), LI_STREAM_RESIZE_REJECTED_INVALID);
  EXPECT_EQ(static_cast<int>(status_e::rejected_unsupported), LI_STREAM_RESIZE_REJECTED_UNSUPPORTED);
  EXPECT_EQ(static_cast<int>(status_e::failed_rolled_back), LI_STREAM_RESIZE_FAILED_ROLLED_BACK);
  EXPECT_EQ(static_cast<int>(status_e::failed_session_ending), LI_STREAM_RESIZE_FAILED_SESSION_ENDING);
}

TEST_F(StreamResizeTest, AResizeRunsEveryStepInOrderAndReportsTheNewMode) {
  auto rig = make_rig();

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  const auto results = rig.sent->snapshot();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].id, 1u);
  EXPECT_EQ(results[0].status, status_e::ok);
  EXPECT_EQ(results[0].mode, k_wide);
  EXPECT_EQ(rig.calls(), k_forward);
  EXPECT_EQ(rig.coordinator->current_mode(), k_wide);
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::idle);
  EXPECT_EQ(rig.end_session_calls(), 0);
}

TEST_F(StreamResizeTest, EveryStepOfATransactionGetsItsGenerationAndTheNextGetsANewOne) {
  auto rig = make_rig();

  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.sent->await(1));
  rig.coordinator->submit({2, k_tall});
  ASSERT_TRUE(rig.sent->await(2));

  std::lock_guard lock {rig.steps->mutex};
  ASSERT_EQ(rig.steps->generations.size(), 8u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(rig.steps->generations[i], rig.steps->generations[0]);
    EXPECT_EQ(rig.steps->generations[i + 4], rig.steps->generations[4]);
  }
  EXPECT_GT(rig.steps->generations[4], rig.steps->generations[0]);
}

TEST_F(StreamResizeTest, ADisplayThatDoesNotComeUpIsOnlyRemoved) {
  auto rig = make_rig({{"create_display", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  const auto results = rig.sent->snapshot();
  EXPECT_EQ(results[0].status, status_e::failed_rolled_back);
  EXPECT_EQ(results[0].mode, k_start);
  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display", "remove_new_display"}));
  EXPECT_EQ(rig.coordinator->current_mode(), k_start);
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::idle);
}

TEST_F(StreamResizeTest, AFailedDisplayConfigurationIsUndoneDesktopFirstThenVideoThenDisplay) {
  auto rig = make_rig({{"switch_topology", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::failed_rolled_back);
  EXPECT_EQ(
    rig.calls(),
    (std::vector<std::string> {"create_display", "switch_topology", "restore_topology", "restore_video", "remove_new_display"})
  );
}

TEST_F(StreamResizeTest, AnEncoderThatDoesNotRunAtTheNewSizeIsRebuiltAtTheOldOne) {
  auto rig = make_rig({{"switch_video", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  const auto results = rig.sent->snapshot();
  EXPECT_EQ(results[0].status, status_e::failed_rolled_back);
  EXPECT_EQ(results[0].mode, k_start);
  EXPECT_EQ(
    rig.calls(),
    (std::vector<std::string> {"create_display", "switch_topology", "switch_video", "restore_topology", "restore_video", "remove_new_display"})
  );

  // The encoder is rebuilt at the mode the stream had, not the one asked for.
  std::lock_guard lock {rig.steps->mutex};
  ASSERT_EQ(rig.steps->modes.size(), 3u);
  EXPECT_EQ(rig.steps->modes[0], k_wide);
  EXPECT_EQ(rig.steps->modes[1], k_wide);
  EXPECT_EQ(rig.steps->modes[2], k_start);
}

TEST_F(StreamResizeTest, AnOldDisplayThatWillNotGoIsNotARollback) {
  auto rig = make_rig({{"remove_old_display", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::ok);
  EXPECT_EQ(rig.calls(), k_forward);
  EXPECT_EQ(rig.coordinator->current_mode(), k_wide);
}

TEST_F(StreamResizeTest, ARollbackThatFailsPoisonsTheSessionAndEndsIt) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases {
    {"restore_topology", {"create_display", "switch_topology", "switch_video", "restore_topology"}},
    {"restore_video", {"create_display", "switch_topology", "switch_video", "restore_topology", "restore_video"}},
    {"remove_new_display", {"create_display", "switch_topology", "switch_video", "restore_topology", "restore_video", "remove_new_display"}},
  };

  for (const auto &[failing, expected_calls] : cases) {
    SCOPED_TRACE(failing);
    auto rig = make_rig({{"switch_video", false}, {failing, false}});

    rig.coordinator->submit({1, k_wide});

    ASSERT_TRUE(rig.sent->await(1));
    const auto results = rig.sent->snapshot();
    EXPECT_EQ(results[0].status, status_e::failed_session_ending);
    EXPECT_EQ(results[0].mode, k_start);
    // Nothing past the step that failed is attempted.
    EXPECT_EQ(rig.calls(), expected_calls);
    EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::poisoned);

    {
      std::unique_lock lock {rig.steps->mutex};
      EXPECT_TRUE(rig.steps->cv.wait_for(lock, 5s, [&] {
        return rig.steps->end_session_calls == 1;
      }));
    }

    // Once poisoned, nothing more is asked of the host.
    rig.coordinator->submit({2, k_tall});
    ASSERT_TRUE(rig.sent->await(2));
    EXPECT_EQ(rig.sent->snapshot()[1].status, status_e::rejected_unsupported);
    EXPECT_EQ(rig.calls(), expected_calls);
    EXPECT_EQ(rig.end_session_calls(), 1);
  }
}

TEST_F(StreamResizeTest, AFailedCreateWhoseDisplayWillNotGoAlsoPoisons) {
  auto rig = make_rig({{"create_display", false}, {"remove_new_display", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::failed_session_ending);
  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display", "remove_new_display"}));
}

TEST_F(StreamResizeTest, OnlyTheNewestWaitingRequestRunsAndTheOthersAreSupersededInTurn) {
  auto rig = make_rig({}, "create_display");

  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.await_hold());

  rig.coordinator->submit({2, k_tall});
  rig.coordinator->submit({3, k_small});
  rig.coordinator->submit({4, k_tall});

  // Nothing may be answered before the request that is running: the client
  // would drop its answer as stale and miss that the stream changed.
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(rig.sent->snapshot().empty());

  // The hold only applies to the first transaction's create.
  {
    std::lock_guard lock {rig.steps->mutex};
    rig.steps->hold.clear();
  }
  rig.release();

  ASSERT_TRUE(rig.sent->await(4));
  const auto results = rig.sent->snapshot();
  ASSERT_EQ(results.size(), 4u);

  EXPECT_EQ(results[0].id, 1u);
  EXPECT_EQ(results[0].status, status_e::ok);
  EXPECT_EQ(results[0].mode, k_wide);

  // Superseded answers report what the stream was encoded at when they left.
  EXPECT_EQ(results[1].id, 2u);
  EXPECT_EQ(results[1].status, status_e::superseded);
  EXPECT_EQ(results[1].mode, k_wide);
  EXPECT_EQ(results[2].id, 3u);
  EXPECT_EQ(results[2].status, status_e::superseded);
  EXPECT_EQ(results[2].mode, k_wide);

  EXPECT_EQ(results[3].id, 4u);
  EXPECT_EQ(results[3].status, status_e::ok);
  EXPECT_EQ(results[3].mode, k_tall);

  // Two transactions, not four.
  std::vector<std::string> expected = k_forward;
  expected.insert(expected.end(), k_forward.begin(), k_forward.end());
  EXPECT_EQ(rig.calls(), expected);
}

TEST_F(StreamResizeTest, ARejectionWaitsForTheAnswerAheadOfIt) {
  auto rig = make_rig({}, "switch_video");

  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.await_hold());

  rig.coordinator->reject_invalid(2);
  rig.coordinator->submit({3, {2560, 1440, 120}});

  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(rig.sent->snapshot().empty());

  rig.release();

  ASSERT_TRUE(rig.sent->await(3));
  const auto results = rig.sent->snapshot();
  EXPECT_EQ(results[0].id, 1u);
  EXPECT_EQ(results[0].status, status_e::ok);
  EXPECT_EQ(results[1].id, 2u);
  EXPECT_EQ(results[1].status, status_e::rejected_invalid);
  EXPECT_EQ(results[1].mode, k_wide);
  EXPECT_EQ(results[2].id, 3u);
  EXPECT_EQ(results[2].status, status_e::rejected_unsupported);
}

TEST_F(StreamResizeTest, AFrameRateChangeIsRefusedUnlessSupported) {
  auto rig = make_rig();

  rig.coordinator->submit({1, {1920, 1080, 120}});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::rejected_unsupported);
  EXPECT_EQ(rig.sent->snapshot()[0].mode, k_start);
  EXPECT_TRUE(rig.calls().empty());

  auto supported = make_rig({}, {}, {.fps_change_supported = true});
  supported.coordinator->submit({1, {1920, 1080, 120}});
  ASSERT_TRUE(supported.sent->await(1));
  EXPECT_EQ(supported.sent->snapshot()[0].status, status_e::ok);
}

TEST_F(StreamResizeTest, TheModeTheStreamAlreadyHasNeedsNoTransaction) {
  auto rig = make_rig();

  rig.coordinator->submit({1, k_start});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::ok);
  EXPECT_EQ(rig.sent->snapshot()[0].mode, k_start);
  EXPECT_TRUE(rig.calls().empty());
}

TEST_F(StreamResizeTest, RepeatedAndOutOfOrderRequestIdsAreIgnored) {
  auto rig = make_rig();

  rig.coordinator->submit({5, k_wide});
  ASSERT_TRUE(rig.sent->await(1));

  rig.coordinator->submit({5, k_tall});
  rig.coordinator->submit({3, k_tall});
  rig.coordinator->reject_invalid(4);
  rig.coordinator->submit({6, k_small});

  ASSERT_TRUE(rig.sent->await(2));
  std::this_thread::sleep_for(50ms);
  const auto results = rig.sent->snapshot();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].id, 5u);
  EXPECT_EQ(results[1].id, 6u);
  EXPECT_EQ(results[1].mode, k_small);
}

TEST_F(StreamResizeTest, StoppingMidTransactionUndoesNothingAndAnswersNothing) {
  auto rig = make_rig({}, "switch_video");

  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.await_hold());
  rig.coordinator->submit({2, k_tall});

  rig.coordinator->stop();

  // The session is ending, and ending it restores the configuration and
  // removes every display the lease holds.
  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display", "switch_topology", "switch_video"}));
  EXPECT_TRUE(rig.sent->snapshot().empty());
  EXPECT_EQ(rig.end_session_calls(), 0);

  // Stopped for good.
  rig.coordinator->submit({3, k_small});
  std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(rig.sent->snapshot().empty());
}

TEST_F(StreamResizeTest, TheAnswerToAnotherChangeIsNotTakenForTheOneAwaited) {
  std::vector<stream_resize::video_ack_t> queue {{3, true}, {4, false}, {5, true}};
  auto pop = [&queue](std::chrono::milliseconds) -> std::optional<stream_resize::video_ack_t> {
    if (queue.empty()) {
      return std::nullopt;
    }
    auto ack = queue.front();
    queue.erase(queue.begin());
    return ack;
  };

  EXPECT_EQ(stream_resize::await_video_ack(5, 1s, pop), std::optional<bool> {true});
  EXPECT_TRUE(queue.empty());

  queue = {{3, true}, {4, false}};
  EXPECT_EQ(stream_resize::await_video_ack(4, 1s, pop), std::optional<bool> {false});

  // Only late answers, then nothing: the wait gives up rather than taking one of them.
  queue = {{1, true}, {2, true}};
  EXPECT_EQ(stream_resize::await_video_ack(9, 1s, pop), std::nullopt);
}

TEST_F(StreamResizeTest, WaitingForAVideoAnswerGivesUpInTime) {
  const auto started = std::chrono::steady_clock::now();
  const auto pop = [](std::chrono::milliseconds wait) -> std::optional<stream_resize::video_ack_t> {
    std::this_thread::sleep_for(std::min(wait, 20ms));
    return stream_resize::video_ack_t {1, true};
  };

  EXPECT_EQ(stream_resize::await_video_ack(2, 100ms, pop), std::nullopt);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST_F(StreamResizeTest, AStopBetweenStepsTakesNoFurtherStep) {
  auto rig = make_rig({}, "create_display");
  {
    std::lock_guard lock {rig.steps->mutex};
    rig.steps->ignore_interrupt = true;
  }

  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.await_hold());

  // The display comes up after the session was told to stop. Nothing else is
  // done to the host, and nothing is undone either.
  rig.coordinator->request_stop();
  rig.release();
  rig.coordinator->stop();

  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display"}));
  EXPECT_TRUE(rig.sent->snapshot().empty());
}
