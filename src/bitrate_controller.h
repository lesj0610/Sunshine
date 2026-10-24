/**
 * @file src/bitrate_controller.h
 * @brief Declarations for fitting a stream's bitrate to what the network carries.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace stream {

  /**
   * @brief Picks a bitrate the network can carry, keeping the frame rate and resolution.
   *
   * A network that cannot carry the stream makes frames queue behind each
   * other, so they arrive late or not at all, and input queues behind them.
   * A lower bitrate makes every frame smaller instead: the stream keeps its
   * frame rate, and with it a pointer that moves smoothly, and gets blurrier.
   * Once the network keeps up again, the bitrate climbs back.
   *
   * It reads what every Moonlight client already sends: how many packets of
   * each frame arrived, requests to recover from frames that did not, and the
   * round trip time of the control connection, which grows while queues fill
   * and before packets are lost.
   */
  class bitrate_controller_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock the caller's times come from.

    /**
     * @brief What a change was made for, for the log.
     */
    struct change_t {
      int kbps;  ///< The new bitrate.
      double loss_percent;  ///< Packets lost in the window that decided it.
      int lost_frames;  ///< Frames the client could not recover in that window.
      std::optional<std::chrono::milliseconds> rtt;  ///< Round trip time at the time.
      std::optional<std::chrono::milliseconds> base_rtt;  ///< Round trip time of an idle network.
    };

    /**
     * @brief Video sent to the client so far.
     */
    struct sent_t {
      std::uint32_t block;  ///< The last FEC block sent, as block_id() gives it.
      std::uint32_t packets;  ///< Data and parity packets sent, wrapping around.
    };

    /**
     * @brief Number FEC blocks in the order they are sent.
     *
     * @param frame_index The block's frame.
     * @param block_index The block, of the up to 4 a frame is sent in.
     * @return The number, wrapping around.
     */
    static constexpr std::uint32_t block_id(std::uint32_t frame_index, int block_index) {
      return frame_index << 2 | (block_index & 3);
    }

    /**
     * @param max_kbps The bitrate the client asked for, which is never exceeded.
     * @param now When the stream started.
     */
    bitrate_controller_t(int max_kbps, clock::time_point now);

    /**
     * @brief Catch up with the video sent to the client.
     *
     * @param sent Video sent so far.
     */
    void packets_sent(sent_t sent);

    /**
     * @brief Count a frame the client reported on.
     *
     * @param sent Video sent so far, as the report comes in.
     * @param frame_index The frame.
     * @param block_index The FEC block of it the report is on.
     * @param expected_packets Data and parity packets sent for that block.
     * @param missing_packets Those that did not arrive.
     * @param sequence_numbers The RTP sequence numbers of the last of them that arrived and of the first
     *                         not to arrive in order, as the high and low halves, which tell the packets it is on.
     */
    void frame_received(sent_t sent, std::uint32_t frame_index, int block_index, int expected_packets, int missing_packets, std::uint32_t sequence_numbers);

    /**
     * @brief Count a frame the client had to ask to recover from.
     */
    void frame_lost();

    /**
     * @brief Decide whether the bitrate should change.
     *
     * @param now The current time.
     * @param rtt Round trip time of the control connection, when known.
     * @return The new bitrate and why, or nothing when it stays.
     */
    std::optional<change_t> update(clock::time_point now, std::optional<std::chrono::milliseconds> rtt);

    /**
     * @brief The bitrate the stream should run at.
     *
     * @return Kilobits per second.
     */
    [[nodiscard]] int kbps() const;

    /**
     * @brief The lowest bitrate it goes down to.
     *
     * @return Kilobits per second.
     */
    [[nodiscard]] int floor_kbps() const;

  private:
    int m_max_kbps;
    int m_floor_kbps;
    int m_kbps;

    clock::time_point m_started;
    clock::time_point m_window_start;
    clock::time_point m_last_decrease;
    clock::time_point m_last_change;
    clock::time_point m_last_congestion;

    sent_t m_sent {};
    std::uint32_t m_window_block = 0;  ///< The last FEC block sent before this window.
    std::uint32_t m_previous_block = 0;  ///< The last FEC block sent before the window before it.
    int m_sent_packets = 0;
    int m_missing_packets = 0;
    std::uint32_t m_pending_block = 0;  ///< The FEC block still going out that reports wait for.
    std::vector<std::uint32_t> m_reported_blocks;  ///< The FEC blocks counted, of the window before this one on.
    int m_pending_missing_packets = 0;
    int m_previous_sent_packets = 0;
    int m_previous_missing_packets = 0;
    bool m_previous_lossy = false;  ///< Whether the window before this one was judged lossy already.
    int m_lost_frames = 0;
    std::optional<std::uint32_t> m_last_sequence_numbers;  ///< Those of the last report.

    std::deque<std::pair<clock::time_point, std::chrono::milliseconds>> m_rtt_samples;
    std::optional<std::chrono::milliseconds> m_previous_rtt;
  };

}  // namespace stream
