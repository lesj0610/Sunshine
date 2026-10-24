/**
 * @file src/bitrate_controller.cpp
 * @brief Definitions for fitting a stream's bitrate to what the network carries.
 */
// local includes
#include "bitrate_controller.h"

// standard includes
#include <algorithm>
#include <utility>

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

    /**
     * @brief Whether an FEC block was sent after another.
     */
    bool sent_after(std::uint32_t block, std::uint32_t other) {
      return static_cast<std::int32_t>(block - other) > 0;
    }
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

  void bitrate_controller_t::packets_sent(sent_t sent) {
    m_sent_packets += static_cast<int>(sent.packets - m_sent.packets);
    m_sent = sent;
    if (m_pending_missing_packets && !sent_after(m_pending_block, m_sent.block)) {
      m_missing_packets += std::exchange(m_pending_missing_packets, 0);
      m_reported_blocks.push_back(m_pending_block);
    }
  }

  void bitrate_controller_t::frame_received(sent_t sent, std::uint32_t frame_index, int block_index, int expected_packets, int missing_packets, std::uint32_t sequence_numbers) {
    // Catching up first leaves the block still going out as the only one
    // a report can be on that does not count as sent yet
    packets_sent(sent);

    // A client that finishes a block and then loses the next one entirely
    // reports that one with what it had of the one it finished, which it
    // reported already. Reports on blocks further apart can repeat sequence
    // numbers once those come around again.
    const auto block {block_id(frame_index, block_index)};
    const auto last {std::exchange(m_last_report, std::pair {block, sequence_numbers})};
    if (last && last->second == sequence_numbers && (block == last->first + 1 || block == (last->first | 3) + 1)) {
      return;
    }

    const int missing {std::clamp(missing_packets, 0, std::max(expected_packets, 0))};
    if (missing == 0) {
      return;
    }
    // A client can report a block it gave up on more than once
    if ((m_pending_missing_packets && block == m_pending_block) || std::ranges::find(m_reported_blocks, block) != m_reported_blocks.end()) {
      return;
    }
    // A report can come in while the rest of its block is still going out,
    // before the block counts as sent, so it waits to count with it. Blocks
    // go out one at a time and in order, so that block comes before any
    // other not sent yet, and a report on a later one is not on video sent.
    // Reports on blocks sent at the end of a window come in the next one,
    // and those on blocks sent before that are too late to tell anything.
    if (sent_after(block, m_sent.block)) {
      if (!m_pending_missing_packets || sent_after(m_pending_block, block)) {
        m_pending_block = block;
        m_pending_missing_packets = missing;
      }
    } else if (sent_after(block, m_window_block)) {
      m_missing_packets += missing;
      m_reported_blocks.push_back(block);
    } else if (sent_after(block, m_previous_block)) {
      m_previous_missing_packets += missing;
      m_reported_blocks.push_back(block);
    }
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
      m_window_block = m_previous_block = m_sent.block;
      m_sent_packets = m_missing_packets = m_pending_missing_packets = m_previous_sent_packets = m_previous_missing_packets = m_lost_frames = 0;
      m_previous_lossy = false;
      m_reported_blocks.clear();
      return std::nullopt;
    }
    if (now - m_window_start < WINDOW) {
      return std::nullopt;
    }

    // Clients only report frames that lost packets, so loss is measured
    // against every packet sent, in the window each FEC block was sent in.
    // Reports on blocks sent at the end of the last window come in this one,
    // so that window is judged again with them, unless it was judged lossy already.
    const auto percent {[](int missing_packets, int sent_packets) {
      return sent_packets ? 100.0 * missing_packets / sent_packets : 0.0;
    }};
    const double window_loss_percent {percent(m_missing_packets, m_sent_packets)};
    const double previous_loss_percent {m_previous_lossy ? 0.0 : percent(m_previous_missing_packets, m_previous_sent_packets)};
    const double loss_percent {std::max(window_loss_percent, previous_loss_percent)};
    const int lost_frames {m_lost_frames};
    m_window_start = now;
    m_lost_frames = 0;
    // A window without blocks, as when the stream pauses, leaves the one
    // before it to take the reports that still come in
    if (m_sent.block != m_window_block) {
      m_previous_block = std::exchange(m_window_block, m_sent.block);
      m_previous_sent_packets = std::exchange(m_sent_packets, 0);
      m_previous_missing_packets = std::exchange(m_missing_packets, 0);
      m_previous_lossy = window_loss_percent >= LOSS_PERCENT;
    } else if (previous_loss_percent >= LOSS_PERCENT) {
      m_previous_lossy = true;
    }
    // Reports on blocks sent before that no longer count, the second time either
    std::erase_if(m_reported_blocks, [this](std::uint32_t block) {
      return !sent_after(block, m_previous_block);
    });

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
