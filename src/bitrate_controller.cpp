/**
 * @file src/bitrate_controller.cpp
 * @brief Definitions for fitting a stream's bitrate to what the network carries.
 */
// local includes
#include "bitrate_controller.h"

// standard includes
#include <algorithm>

namespace stream {
  using namespace std::literals;

  namespace {
    constexpr auto WINDOW = 500ms;  ///< How long a stretch of reports is judged over.
    constexpr auto WARMUP = 2s;  ///< The round trip time settles for a moment after connecting.
    constexpr auto DECREASE_INTERVAL = 1s;  ///< A lower bitrate needs a moment to drain the queue before it shows.
    constexpr auto CLEAN_BEFORE_INCREASE = 2s;  ///< How long the network has to keep up before the bitrate climbs.
    constexpr auto BASE_RTT_WINDOW = 30s;  ///< How far back the round trip time of an idle network is looked for.
    constexpr auto MIN_QUEUE_DELAY = 50ms;  ///< Queues shorter than this are not worth a blurrier picture.

    constexpr double LOSS_PERCENT = 2.0;  ///< Packet loss that counts as congestion.
    constexpr double SEVERE_LOSS_PERCENT = 10.0;  ///< Packet loss that calls for a bigger step.
    constexpr int SEVERE_LOST_FRAMES = 3;  ///< Unrecovered frames in a window that call for a bigger step.
    constexpr double DECREASE = 0.7;  ///< Step down on congestion.
    constexpr double SEVERE_DECREASE = 0.5;  ///< Step down on heavy congestion.
    constexpr double INCREASE = 1.15;  ///< Step up once the network keeps up.
    constexpr int FLOOR_PERCENT = 20;  ///< Lowest bitrate, as a share of the requested one.
    constexpr int MIN_FLOOR_KBPS = 1000;  ///< Lowest bitrate, however low the requested one.
  }  // namespace

  bitrate_controller_t::bitrate_controller_t(int max_kbps, clock::time_point now):
      m_max_kbps {std::max(max_kbps, 1)},
      m_floor_kbps {std::min(m_max_kbps, std::max(m_max_kbps * FLOOR_PERCENT / 100, MIN_FLOOR_KBPS))},
      m_kbps {m_max_kbps},
      m_started {now},
      m_window_start {now},
      m_last_decrease {now - DECREASE_INTERVAL},
      m_last_change {now},
      m_last_congestion {now} {
  }

  void bitrate_controller_t::frame_received(int expected_packets, int missing_packets) {
    if (expected_packets <= 0) {
      return;
    }
    m_expected_packets += expected_packets;
    m_missing_packets += std::clamp(missing_packets, 0, expected_packets);
  }

  void bitrate_controller_t::frame_lost() {
    ++m_lost_frames;
  }

  std::optional<bitrate_controller_t::change_t> bitrate_controller_t::update(clock::time_point now, std::optional<std::chrono::milliseconds> rtt) {
    const bool settled {now - m_started >= WARMUP};
    if (rtt && settled) {
      m_rtt_samples.emplace_back(now, *rtt);
    }
    while (!m_rtt_samples.empty() && now - m_rtt_samples.front().first > BASE_RTT_WINDOW) {
      m_rtt_samples.pop_front();
    }

    if (!settled) {
      // Starting a stream asks for a keyframe while the network settles,
      // which says nothing about whether it can carry the stream
      m_window_start = now;
      m_expected_packets = m_missing_packets = m_lost_frames = 0;
      return std::nullopt;
    }
    if (now - m_window_start < WINDOW) {
      return std::nullopt;
    }

    const double loss_percent {m_expected_packets ? 100.0 * m_missing_packets / m_expected_packets : 0.0};
    const int lost_frames {m_lost_frames};
    m_window_start = now;
    m_expected_packets = m_missing_packets = m_lost_frames = 0;

    std::optional<std::chrono::milliseconds> base_rtt;
    if (!m_rtt_samples.empty()) {
      base_rtt = std::ranges::min_element(m_rtt_samples, {}, &decltype(m_rtt_samples)::value_type::second)->second;
    }

    // A queue still filling is congestion. One that is draining after the
    // last step down is not, though the averaged round trip time is still high.
    const auto previous_rtt {std::exchange(m_previous_rtt, rtt)};
    const bool queued {rtt && base_rtt && *rtt >= *base_rtt + std::max(MIN_QUEUE_DELAY, *base_rtt / 2) && (!previous_rtt || *rtt >= *previous_rtt)};

    const bool severe {loss_percent >= SEVERE_LOSS_PERCENT || lost_frames >= SEVERE_LOST_FRAMES};
    const bool congested {severe || loss_percent >= LOSS_PERCENT || lost_frames > 0 || queued};

    const auto change {[&](int kbps) -> std::optional<change_t> {
      if (kbps == m_kbps) {
        return std::nullopt;
      }
      m_kbps = kbps;
      m_last_change = now;
      return change_t {kbps, loss_percent, lost_frames, rtt, base_rtt};
    }};

    if (congested) {
      m_last_congestion = now;
      if (now - m_last_decrease < DECREASE_INTERVAL) {
        return std::nullopt;
      }
      m_last_decrease = now;
      return change(std::max(m_floor_kbps, static_cast<int>(m_kbps * (severe ? SEVERE_DECREASE : DECREASE))));
    }

    if (m_kbps < m_max_kbps && now - m_last_congestion >= CLEAN_BEFORE_INCREASE && now - m_last_change >= CLEAN_BEFORE_INCREASE) {
      return change(std::min(m_max_kbps, std::max(m_kbps + 1, static_cast<int>(m_kbps * INCREASE))));
    }
    return std::nullopt;
  }

  int bitrate_controller_t::kbps() const {
    return m_kbps;
  }

  int bitrate_controller_t::floor_kbps() const {
    return m_floor_kbps;
  }

}  // namespace stream
