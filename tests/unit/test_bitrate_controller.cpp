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
    bitrate_controller_t::sent_t sent {};  ///< Video sent so far, a frame of one FEC block every 100ms.
    std::uint32_t frame_index = 0;  ///< The last frame sent.

    /**
     * @brief Hand over a report on a frame, with the video sent as it comes in.
     *
     * @param frame_index The frame.
     * @param block_index The FEC block of it the report is on.
     * @param expected_packets Data and parity packets sent for that block.
     * @param missing_packets Those that did not arrive.
     */
    void report(std::uint32_t frame_index, int block_index, int expected_packets, int missing_packets) {
      controller.frame_received(sent, frame_index, block_index, expected_packets, missing_packets);
    }

    /**
     * @brief Run the stream for a while over a network of one kind.
     *
     * @param duration How long.
     * @param missing Packets missing out of every 100.
     * @param rtt Round trip time.
     * @param lost_frames Frames the client asks to recover, every 100ms.
     * @param sent Packets sent every 100ms.
     * @return The bitrates the controller moved to.
     */
    std::vector<int> run(std::chrono::milliseconds duration, int missing, std::chrono::milliseconds rtt, int lost_frames = 0, int sent = 100) {
      std::vector<int> changes;
      for (const auto end = now + duration; now < end;) {
        now += 100ms;
        if (sent) {
          this->sent.block = bitrate_controller_t::block_id(++frame_index, 0);
          this->sent.packets += sent;
        }
        controller.packets_sent(this->sent);
        // Clients only report frames that lost packets
        if (missing) {
          report(frame_index, 0, 100, missing);
        }
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

TEST(BitrateControllerTest, StepsDownAtTwoPercentLossAndHalvesAtTen) {
  for (const auto &[missing, kbps] : {std::pair {1, 10000}, std::pair {2, 7000}, std::pair {10, 5000}}) {
    network_t network;
    // Up to the end of a window
    network.run(2900ms, 0, 20ms);
    network.run(500ms, missing, 20ms);
    EXPECT_EQ(network.controller.kbps(), kbps);
  }
}

TEST(BitrateControllerTest, MeasuresLossAgainstEveryPacketSent) {
  network_t network;
  network.run(3s, 0, 20ms);

  // One packet of a frame of 8 is one of the 1000 sent in a window, not one in 8
  for (int x = 0; x < 10; ++x) {
    network.report(network.frame_index, 0, 8, 1);
    EXPECT_TRUE(network.run(500ms, 0, 20ms, 0, 200).empty());
  }
}

TEST(BitrateControllerTest, CountsALateReportAgainstTheWindowItWasSentIn) {
  for (const auto sent : {8, 0}) {
    network_t network;
    // Up to the end of a window of 1000 packets
    network.run(2900ms, 0, 20ms, 0, 200);

    // The stream slows down or pauses, and reports on a frame sent before that come in
    const auto frame_index {network.frame_index};
    network.report(frame_index, 0, 8, 1);
    EXPECT_TRUE(network.run(100ms, 0, 20ms, 0, sent).empty());
    EXPECT_TRUE(network.run(2s, 0, 20ms, 0, 0).empty());
    network.report(frame_index, 0, 8, 1);
    EXPECT_TRUE(network.run(2s, 0, 20ms).empty());
  }
}

TEST(BitrateControllerTest, JudgesEachWindowOnItsOwnLoss) {
  for (const auto &[missing, kbps] : {std::pair {1, 10000}, std::pair {5, 7000}}) {
    network_t network;
    // Up to the end of a window of 500 packets
    network.run(2900ms, 0, 20ms);

    // The reports on it come in during the next window, which loses as much
    network.report(network.frame_index, 0, 100, 5 * missing);
    network.run(500ms, missing, 20ms);
    EXPECT_EQ(network.controller.kbps(), kbps);
  }
}

TEST(BitrateControllerTest, AddsLateReportsToTheOthersOfTheirWindow) {
  // Of a window of 500 packets, some reports come in the next window
  for (const auto &[on_time, late, kbps] : {std::tuple {6, 6, 7000}, std::tuple {5, 55, 5000}}) {
    network_t network;
    network.run(2900ms, 0, 20ms);

    network.run(400ms, 0, 20ms);
    network.report(network.frame_index - 1, 0, 100, on_time);
    const auto frame_index {network.frame_index};
    network.run(100ms, 0, 20ms);
    network.report(frame_index, 0, 100, late);
    network.run(500ms, 0, 20ms);
    EXPECT_EQ(network.controller.kbps(), kbps);
  }
}

TEST(BitrateControllerTest, CountsEachFecBlockInTheWindowItWasSentIn) {
  network_t network;
  // Up to the end of a window of 100 packets, the last of them the first FEC block of a frame
  network.run(2400ms, 0, 20ms);
  network.run(500ms, 0, 20ms, 0, 20);

  // Its second block opens the next window of 500 packets
  network.sent.block = bitrate_controller_t::block_id(network.frame_index, 1);
  network.sent.packets += 100;
  network.report(network.frame_index, 1, 100, 2);
  EXPECT_TRUE(network.run(500ms, 0, 20ms, 0, 80).empty());
}

TEST(BitrateControllerTest, CountsAReportWithTheBlockItComesInAheadOf) {
  network_t network;
  network.run(2400ms, 0, 20ms);
  network.run(400ms, 0, 20ms, 0, 25);

  // The report on the next block comes in while the block is still going
  // out, and the window of 100 packets ends before the block counts as sent
  network.report(network.frame_index + 1, 0, 52, 2);
  EXPECT_TRUE(network.run(100ms, 0, 20ms, 0, 0).empty());

  // It counts in the window of 500 packets the block is counted in
  EXPECT_TRUE(network.run(100ms, 0, 20ms, 0, 52).empty());
  EXPECT_TRUE(network.run(400ms, 0, 20ms, 0, 112).empty());
}

TEST(BitrateControllerTest, CountsReportsOnBlocksSentAndGoingOutApart) {
  for (const bool reversed : {false, true}) {
    network_t network;
    network.run(2400ms, 0, 20ms);
    network.run(400ms, 0, 20ms, 0, 225);

    // The first block of a frame is sent, ending a window of 1000 packets,
    // and the second is still going out, when reports on both come in
    const auto frame_index {++network.frame_index};
    network.sent.block = bitrate_controller_t::block_id(frame_index, 0);
    network.sent.packets += 100;
    for (const int block_index : {reversed ? 1 : 0, reversed ? 0 : 1}) {
      network.report(frame_index, block_index, 100, 1);
    }
    EXPECT_TRUE(network.run(100ms, 0, 20ms, 0, 0).empty());

    // The second block is a window of 100 packets on its own
    network.sent.block = bitrate_controller_t::block_id(frame_index, 1);
    network.sent.packets += 100;
    EXPECT_TRUE(network.run(500ms, 0, 20ms, 0, 0).empty());
  }
}

TEST(BitrateControllerTest, CountsABlockReportedTwiceOnce) {
  network_t network;
  network.run(2900ms, 0, 20ms);

  // 8 of a window of 500 packets go missing, in a block the client reports twice
  network.run(400ms, 0, 20ms);
  network.report(network.frame_index, 0, 100, 8);
  network.report(network.frame_index, 0, 100, 8);
  EXPECT_TRUE(network.run(100ms, 0, 20ms).empty());
}

TEST(BitrateControllerTest, KeepsSteppingDownWhenALateReportPrecedesHeavyLoss) {
  network_t network;
  network.run(2900ms, 0, 20ms);

  network.report(network.frame_index, 0, 100, 10);
  EXPECT_EQ(network.run(10s, 10, 20ms), (std::vector<int> {5000, 2500, 2000}));
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
