/**
 * @file tests/unit/test_launch_reservation.cpp
 * @brief Test that only one launch at a time can get as far as doing anything.
 *
 * Starting an app stops whichever one is already running, so a request that
 * is going to lose the race for the pending slot has to find that out before
 * it starts one, not after.
 */
// standard includes
#include <atomic>
#include <future>
#include <thread>
#include <vector>

// test includes
#include "../tests_common.h"

// local includes
#include <src/rtsp.h>

namespace {

  using namespace std::chrono_literals;

}  // namespace

class LaunchReservationTest: public ::testing::Test {};

TEST_F(LaunchReservationTest, OnlyOneClaimIsHandedOutAtATime) {
  auto first = rtsp_stream::reserve_launch_session();
  ASSERT_TRUE(static_cast<bool>(first));

  auto second = rtsp_stream::reserve_launch_session();
  EXPECT_FALSE(static_cast<bool>(second));
}

TEST_F(LaunchReservationTest, AClaimThatIsNotUsedGoesBack) {
  {
    auto claim = rtsp_stream::reserve_launch_session();
    ASSERT_TRUE(static_cast<bool>(claim));
  }

  auto next = rtsp_stream::reserve_launch_session();
  EXPECT_TRUE(static_cast<bool>(next));
}

TEST_F(LaunchReservationTest, AMovedClaimIsOnlyGivenBackOnce) {
  {
    auto claim = rtsp_stream::reserve_launch_session();
    ASSERT_TRUE(static_cast<bool>(claim));

    auto moved = std::move(claim);
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_FALSE(static_cast<bool>(claim));
  }

  auto next = rtsp_stream::reserve_launch_session();
  EXPECT_TRUE(static_cast<bool>(next));
}

TEST_F(LaunchReservationTest, OnlyOneOfManyRacingLaunchesGetsThrough) {
  // What matters is that the losers learn it before they have done anything,
  // so nothing of theirs has to be undone and nothing of the winner's is
  // taken down by them.
  constexpr int racers = 8;

  std::atomic<int> winners {0};
  std::atomic<bool> go {false};
  std::vector<std::future<void>> threads;

  for (int i = 0; i < racers; ++i) {
    threads.push_back(std::async(std::launch::async, [&winners, &go] {
      while (!go) {
        std::this_thread::yield();
      }

      auto claim = rtsp_stream::reserve_launch_session();
      if (claim) {
        winners += 1;
        // Held for a moment, the way a real launch holds it while it starts
        // an app and prepares a display.
        std::this_thread::sleep_for(20ms);
      }
    }));
  }

  go = true;
  for (auto &thread : threads) {
    thread.get();
  }

  EXPECT_EQ(winners.load(), 1);

  // And the slot is free again once they are all done.
  auto next = rtsp_stream::reserve_launch_session();
  EXPECT_TRUE(static_cast<bool>(next));
}
