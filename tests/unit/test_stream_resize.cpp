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
#include <thread>
#include <vector>

// test includes
#include "../tests_common.h"

// local includes
#include <src/stream_resize.h>
#include <src/video.h>

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

  // The first frames the fake encoder reports for the new mode and for the old one
  constexpr std::uint32_t k_switched_at = 500;
  constexpr std::uint32_t k_restored_at = 700;

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
      std::map<std::string, std::chrono::milliseconds> delay;  ///< Step name to how long it takes.
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
      state->cv.notify_all();

      if (const auto delay = state->delay.find(name); delay != state->delay.end()) {
        const auto how_long = delay->second;
        lock.unlock();
        std::this_thread::sleep_for(how_long);
        lock.lock();
      }

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

    std::optional<std::uint32_t> switch_video(std::uint64_t generation, const stream_mode_t &mode) override {
      if (!step("switch_video", generation, mode)) {
        return std::nullopt;
      }
      return k_switched_at;
    }

    bool remove_old_display(std::uint64_t generation) override {
      return step("remove_old_display", generation);
    }

    bool restore_topology(std::uint64_t generation) override {
      return step("restore_topology", generation);
    }

    std::optional<std::uint32_t> restore_video(std::uint64_t generation, const stream_mode_t &mode) override {
      if (!step("restore_video", generation, mode)) {
        return std::nullopt;
      }
      return k_restored_at;
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

    /// Wait until the transaction running is done with every step, including the ones after its answer.
    bool await_idle() {
      const auto give_up_at = std::chrono::steady_clock::now() + 5s;
      while (coordinator->phase() != stream_resize::phase_e::idle) {
        if (std::chrono::steady_clock::now() >= give_up_at) {
          return false;
        }
        std::this_thread::sleep_for(1ms);
      }
      return true;
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
  ASSERT_TRUE(rig.await_idle());
  const auto results = rig.sent->snapshot();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].id, 1u);
  EXPECT_EQ(results[0].status, status_e::ok);
  EXPECT_EQ(results[0].mode, k_wide);
  EXPECT_EQ(results[0].first_frame, std::optional<std::uint32_t> {k_switched_at});
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
  ASSERT_TRUE(rig.await_idle());

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
  // The stream was never touched, so any keyframe of it will do
  EXPECT_FALSE(results[0].first_frame.has_value());
  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display", "remove_new_display"}));
  EXPECT_EQ(rig.coordinator->current_mode(), k_start);
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::idle);
}

TEST_F(StreamResizeTest, AFailedDisplayConfigurationIsUndoneDesktopFirstThenVideoThenDisplay) {
  auto rig = make_rig({{"switch_topology", false}});

  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::failed_rolled_back);
  EXPECT_EQ(rig.sent->snapshot()[0].first_frame, std::optional<std::uint32_t> {k_restored_at});
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
  // Frames of the failed attempt can still arrive, so the old mode is taken up from the restored encoder's first frame
  EXPECT_EQ(results[0].first_frame, std::optional<std::uint32_t> {k_restored_at});
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
  ASSERT_TRUE(rig.await_idle());
  EXPECT_EQ(rig.sent->snapshot()[0].status, status_e::ok);
  EXPECT_EQ(rig.calls(), k_forward);
  EXPECT_EQ(rig.coordinator->current_mode(), k_wide);
}

TEST_F(StreamResizeTest, AnOkIsAnsweredBeforeTheOldDisplayIsRemoved) {
  auto rig = make_rig({}, "remove_old_display");

  rig.coordinator->submit({1, k_wide});

  // The new display is streamed once the encoder runs on it, so the client
  // hears then, not after the old display is gone
  ASSERT_TRUE(rig.sent->await(1));
  ASSERT_TRUE(rig.await_hold());
  const auto results = rig.sent->snapshot();
  EXPECT_EQ(results[0].status, status_e::ok);
  EXPECT_EQ(results[0].first_frame, std::optional<std::uint32_t> {k_switched_at});
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::committed);
  EXPECT_EQ(rig.coordinator->current_mode(), k_wide);

  // A request arriving meanwhile waits for the old display to go
  rig.coordinator->submit({2, k_tall});
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rig.calls(), k_forward);

  {
    std::lock_guard lock {rig.steps->mutex};
    rig.steps->hold.clear();
  }
  rig.release();
  ASSERT_TRUE(rig.sent->await(2));
  ASSERT_TRUE(rig.await_idle());
  EXPECT_EQ(rig.sent->snapshot()[1].mode, k_tall);
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
  ASSERT_TRUE(rig.await_idle());
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
  EXPECT_FALSE(rig.sent->snapshot()[0].first_frame.has_value());
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
  std::vector<stream_resize::video_ack_t> queue {{3, true, 30}, {4, false}, {5, true, 50}};
  auto pop = [&queue](std::chrono::milliseconds) -> std::optional<stream_resize::video_ack_t> {
    if (queue.empty()) {
      return std::nullopt;
    }
    auto ack = queue.front();
    queue.erase(queue.begin());
    return ack;
  };

  auto ack = stream_resize::await_video_ack(5, 1s, pop);
  ASSERT_TRUE(ack.has_value());
  EXPECT_TRUE(ack->applied);
  EXPECT_EQ(ack->first_frame, 50u);
  EXPECT_TRUE(queue.empty());

  queue = {{3, true, 30}, {4, false}};
  ack = stream_resize::await_video_ack(4, 1s, pop);
  ASSERT_TRUE(ack.has_value());
  EXPECT_FALSE(ack->applied);

  // Only late answers, then nothing: the wait gives up rather than taking one of them.
  queue = {{1, true, 10}, {2, true, 20}};
  EXPECT_FALSE(stream_resize::await_video_ack(9, 1s, pop).has_value());
}

TEST_F(StreamResizeTest, WaitingForAVideoAnswerGivesUpInTime) {
  const auto started = std::chrono::steady_clock::now();
  const auto pop = [](std::chrono::milliseconds wait) -> std::optional<stream_resize::video_ack_t> {
    std::this_thread::sleep_for(std::min(wait, 20ms));
    return stream_resize::video_ack_t {1, true};
  };

  EXPECT_FALSE(stream_resize::await_video_ack(2, 100ms, pop).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST_F(StreamResizeTest, ASlowRollbackIsAnsweredForWhatItDid) {
  // Every step takes its time, but the whole rollback fits in the time a
  // request may take, so the client hears it was rolled back and the session
  // carries on
  constexpr auto step_time = 100ms;
  auto rig = make_rig({{"switch_video", false}}, {}, {.decide_within = 1500ms});
  {
    std::lock_guard lock {rig.steps->mutex};
    for (const auto *name : {"create_display", "switch_topology", "switch_video", "restore_topology", "restore_video", "remove_new_display"}) {
      rig.steps->delay[name] = step_time;
    }
  }

  const auto started = std::chrono::steady_clock::now();
  rig.coordinator->submit({1, k_wide});

  ASSERT_TRUE(rig.sent->await(1));
  EXPECT_GE(std::chrono::steady_clock::now() - started, 6 * step_time);
  const auto results = rig.sent->snapshot();
  EXPECT_EQ(results[0].status, status_e::failed_rolled_back);
  EXPECT_EQ(results[0].first_frame, std::optional<std::uint32_t> {k_restored_at});

  // Nothing more comes of it once the time is up
  std::this_thread::sleep_for(1600ms - (std::chrono::steady_clock::now() - started));
  EXPECT_EQ(rig.sent->snapshot().size(), 1u);
  EXPECT_FALSE(rig.coordinator->timed_out());
  EXPECT_EQ(rig.end_session_calls(), 0);
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::idle);
}

TEST_F(StreamResizeTest, AStuckStepEndsTheSessionInTime) {
  // The display configuration never comes back, not even when interrupted
  constexpr auto decide_within = 300ms;
  auto rig = make_rig({}, "switch_topology", {.decide_within = decide_within});
  {
    std::lock_guard lock {rig.steps->mutex};
    rig.steps->ignore_interrupt = true;
  }

  const auto started = std::chrono::steady_clock::now();
  rig.coordinator->submit({1, k_wide});
  ASSERT_TRUE(rig.await_hold());
  rig.coordinator->submit({2, k_tall});

  // Both are answered, in order, and the session ends
  ASSERT_TRUE(rig.sent->await(2));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, decide_within);
  EXPECT_LT(elapsed, decide_within + 2s);
  const auto results = rig.sent->snapshot();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].id, 1u);
  EXPECT_EQ(results[0].status, status_e::failed_session_ending);
  EXPECT_EQ(results[1].id, 2u);
  EXPECT_EQ(results[1].status, status_e::failed_session_ending);
  EXPECT_TRUE(rig.coordinator->timed_out());
  EXPECT_EQ(rig.coordinator->phase(), stream_resize::phase_e::poisoned);
  {
    std::unique_lock lock {rig.steps->mutex};
    EXPECT_TRUE(rig.steps->cv.wait_for(lock, 5s, [&] {
      return rig.steps->end_session_calls == 1;
    }));
    EXPECT_TRUE(rig.steps->interrupted);
  }

  // The step coming back late changes nothing: no further step, no second answer
  rig.release();
  rig.coordinator->submit({3, k_small});
  std::this_thread::sleep_for(50ms);
  rig.coordinator->stop();
  EXPECT_EQ(rig.calls(), (std::vector<std::string> {"create_display", "switch_topology"}));
  EXPECT_EQ(rig.sent->snapshot().size(), 2u);
  EXPECT_EQ(rig.end_session_calls(), 1);
}

TEST_F(StreamResizeTest, TheTimeToDecideLeavesTheAnswerTimeToGetOut) {
  EXPECT_EQ(stream_resize::answer_within, std::chrono::milliseconds {LI_STREAM_RESIZE_ANSWER_WITHIN_MS});
  EXPECT_LE(stream_resize::options_t {}.decide_within + stream_resize::outbox_t::give_up_after, stream_resize::answer_within);
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

TEST_F(StreamResizeTest, AnswersThatFailToGoOutAreKeptInOrderAndRetried) {
  stream_resize::outbox_t outbox;
  const auto start = std::chrono::steady_clock::now();
  outbox.push({1, k_wide, status_e::ok});
  outbox.push({2, k_wide, status_e::superseded});
  outbox.push({3, k_tall, status_e::ok});

  std::vector<std::uint32_t> sent;
  bool line_down = true;
  const auto send = [&](const stream_resize::result_t &result) {
    if (line_down && result.id == 2) {
      return false;
    }
    sent.push_back(result.id);
    return true;
  };

  // The second one does not go out, so the third waits behind it
  EXPECT_TRUE(outbox.flush(send, start));
  EXPECT_EQ(sent, (std::vector<std::uint32_t> {1}));
  EXPECT_FALSE(outbox.empty());

  line_down = false;
  EXPECT_TRUE(outbox.flush(send, start + 1s));
  EXPECT_EQ(sent, (std::vector<std::uint32_t> {1, 2, 3}));
  EXPECT_TRUE(outbox.empty());
}

TEST_F(StreamResizeTest, AnswersThatKeepFailingEndTheSession) {
  stream_resize::outbox_t outbox;
  const auto start = std::chrono::steady_clock::now();
  outbox.push({1, k_wide, status_e::ok});
  const auto refuse = [](const stream_resize::result_t &) {
    return false;
  };

  EXPECT_TRUE(outbox.flush(refuse, start));
  EXPECT_TRUE(outbox.flush(refuse, start + stream_resize::outbox_t::give_up_after - 1ms));
  EXPECT_FALSE(outbox.flush(refuse, start + stream_resize::outbox_t::give_up_after));

  // A success in between starts the clock again
  stream_resize::outbox_t recovering;
  recovering.push({1, k_wide, status_e::ok});
  recovering.push({2, k_tall, status_e::ok});
  bool first = true;
  const auto one_then_none = [&](const stream_resize::result_t &) {
    return std::exchange(first, false);
  };
  EXPECT_TRUE(recovering.flush(refuse, start));
  EXPECT_TRUE(recovering.flush(one_then_none, start + 4s));
  EXPECT_TRUE(recovering.flush(refuse, start + 8s));
  EXPECT_FALSE(recovering.flush(refuse, start + 8s + stream_resize::outbox_t::give_up_after));
}

TEST_F(StreamResizeTest, TheDisplayAResizeNamesWinsWhileTheOldOneIsStillThere) {
  // Both the old and the new display are on the list: the old one is only
  // removed once the new one is streamed.
  const std::vector<std::string> names {"\\\\.\\DISPLAY1", "\\\\.\\DISPLAY7", "\\\\.\\DISPLAY8"};
  const int capturing_old = 1;

  EXPECT_EQ(video::choose_display(names, capturing_old, std::string {"\\\\.\\DISPLAY8"}), 2);

  // Without a resize the display being captured stays, as before
  EXPECT_EQ(video::choose_display(names, capturing_old, std::nullopt), capturing_old);

  // A display that is not there leaves the pick alone
  EXPECT_EQ(video::choose_display(names, capturing_old, std::string {"\\\\.\\DISPLAY9"}), capturing_old);
}

TEST_F(StreamResizeTest, AResizeIsOnlyAnsweredOnceAFrameOfItsDisplayWasEncoded) {
  video::config_change_progress_t progress {{7, 2560, 1080, 60, "\\\\.\\DISPLAY8"}};

  // Still capturing the old display, which is still there
  progress.capturing("\\\\.\\DISPLAY7");
  EXPECT_FALSE(progress.on_target());
  EXPECT_FALSE(progress.frame_encoded(true).has_value());

  // On the new display, a frame made up while waiting for the capture is not enough
  progress.capturing("\\\\.\\DISPLAY8");
  EXPECT_TRUE(progress.on_target());
  EXPECT_FALSE(progress.frame_encoded(false).has_value());

  // Nor is one from before an encoder at the new size said where it starts
  EXPECT_FALSE(progress.frame_encoded(true).has_value());

  // An encoder made again after a reinit still encodes the new size, so the
  // first one's first frame stays the answer's
  progress.started(41);
  progress.started(57);
  const auto ack = progress.frame_encoded(true);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->generation, 7u);
  EXPECT_TRUE(ack->applied);
  EXPECT_EQ(ack->first_frame, 41u);

  // The capture moving away again takes the answer back
  progress.capturing("\\\\.\\DISPLAY7");
  EXPECT_FALSE(progress.frame_encoded(true).has_value());

  const auto no = progress.failed();
  EXPECT_EQ(no.generation, 7u);
  EXPECT_FALSE(no.applied);

  // A change that names no display takes whichever is captured
  video::config_change_progress_t anywhere {{8, 1920, 1080, 60, {}}};
  EXPECT_TRUE(anywhere.on_target());
  anywhere.started(0x1'0000'0005);
  ASSERT_TRUE(anywhere.frame_encoded(true).has_value());
  // Frame numbers go out as their low 32 bits
  EXPECT_EQ(anywhere.frame_encoded(true)->first_frame, 5u);
}
