/**
 * @file src/bitrate_controller.h
 * @brief Declarations for fitting a stream's bitrate to what the network carries.
 */
#pragma once

// standard includes
#include <chrono>
#include <deque>
#include <optional>
#include <utility>

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
     * @param max_kbps The bitrate the client asked for, which is never exceeded.
     * @param now When the stream started.
     */
    bitrate_controller_t(int max_kbps, clock::time_point now);

    /**
     * @brief Count a frame the client reported on.
     *
     * @param expected_packets Data and parity packets sent for it.
     * @param missing_packets Those that did not arrive.
     */
    void frame_received(int expected_packets, int missing_packets);

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

    int m_expected_packets = 0;
    int m_missing_packets = 0;
    int m_lost_frames = 0;

    std::deque<std::pair<clock::time_point, std::chrono::milliseconds>> m_rtt_samples;
    std::optional<std::chrono::milliseconds> m_previous_rtt;
  };

}  // namespace stream
