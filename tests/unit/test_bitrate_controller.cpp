/**
 * @file tests/unit/test_bitrate_controller.cpp
 * @brief Test fitting the bitrate to what the network carries.
 */
#include "../tests_common.h"

#include <src/bitrate_controller.h>

using namespace std::literals;
using stream::bitrate_controller_t;

namespace {

  /**
   * @brief Feeds a controller the way the control thread does, every 100ms.
   */
  struct network_t {
    bitrate_controller_t controller {10000, {}};
    bitrate_controller_t::clock::time_point now {};

    /**
     * @brief Run the stream for a while over a network of one kind.
     *
     * @param duration How long.
     * @param missing Packets missing out of every 100.
     * @param rtt Round trip time.
     * @param lost_frames Frames the client asks to recover, every 100ms.
     * @return The bitrates the controller moved to.
     */
    std::vector<int> run(std::chrono::milliseconds duration, int missing, std::chrono::milliseconds rtt, int lost_frames = 0) {
      std::vector<int> changes;
      for (const auto end = now + duration; now < end;) {
        now += 100ms;
        controller.frame_received(100, missing);
        for (int x = 0; x < lost_frames; ++x) {
          controller.frame_lost();
        }
        if (const auto change = controller.update(now, rtt)) {
          changes.push_back(change->kbps);
        }
      }
      return changes;
    }
  };

}  // namespace

TEST(BitrateControllerTest, KeepsTheRequestedBitrateOnANetworkThatKeepsUp) {
  network_t network;
  EXPECT_TRUE(network.run(30s, 0, 20ms).empty());
  EXPECT_EQ(network.controller.kbps(), 10000);
}

TEST(BitrateControllerTest, IgnoresTheStartOfTheStream) {
  // The client asks for a keyframe as the stream starts
  network_t network;
  EXPECT_TRUE(network.run(1900ms, 50, 20ms, 1).empty());
}

TEST(BitrateControllerTest, StepsDownOncePerSecondAtMostAndNoLowerThanTheFloor) {
  network_t network;
  network.run(3s, 0, 20ms);
  EXPECT_EQ(network.run(3s, 5, 20ms), (std::vector<int> {7000, 4900, 3430}));
  EXPECT_EQ(network.run(10s, 5, 20ms), (std::vector<int> {2401, 2000}));
  EXPECT_EQ(network.controller.kbps(), network.controller.floor_kbps());
}

TEST(BitrateControllerTest, HalvesOnHeavyLossOrLostFrames) {
  for (const auto &[missing, lost_frames] : {std::pair {20, 0}, std::pair {0, 1}}) {
    network_t network;
    network.run(3s, 0, 20ms);
    EXPECT_EQ(network.run(500ms, missing, 20ms, lost_frames), std::vector<int> {5000});
  }
}

TEST(BitrateControllerTest, StepsDownWhileAQueueFillsButNotWhileItDrains) {
  network_t network;
  network.run(3s, 0, 20ms);
  EXPECT_EQ(network.run(1s, 0, 200ms), std::vector<int> {7000});

  // Lower each time it is judged, so the queue is emptying
  for (const auto rtt : {180ms, 150ms, 110ms, 80ms}) {
    for (const auto kbps : network.run(500ms, 0, rtt)) {
      EXPECT_GT(kbps, 7000);
    }
  }
}

TEST(BitrateControllerTest, ClimbsBackOnceTheNetworkKeepsUp) {
  network_t network;
  network.run(3s, 0, 20ms);
  network.run(500ms, 20, 20ms);
  ASSERT_EQ(network.controller.kbps(), 5000);

  const auto changes = network.run(60s, 0, 20ms);
  EXPECT_TRUE(std::ranges::is_sorted(changes));
  EXPECT_EQ(network.controller.kbps(), 10000);
}
