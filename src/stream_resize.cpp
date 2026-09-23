/**
 * @file src/stream_resize.cpp
 * @brief Definitions for resizing a running stream without reconnecting.
 */
// header include
#include "stream_resize.h"

// standard includes
#include <algorithm>
#include <string_view>
#include <utility>

// local includes
#include "logging.h"

using namespace std::literals;

namespace stream_resize {

  namespace {

    /**
     * @brief Name a status for the log.
     *
     * @param status Status to name.
     * @return A short name.
     */
    std::string_view to_string(status_e status) {
      switch (status) {
        case status_e::ok:
          return "OK"sv;
        case status_e::superseded:
          return "superseded"sv;
        case status_e::rejected_invalid:
          return "rejected as invalid"sv;
        case status_e::rejected_unsupported:
          return "rejected as unsupported"sv;
        case status_e::failed_rolled_back:
          return "failed and rolled back"sv;
        case status_e::failed_session_ending:
          return "failed, ending the session"sv;
      }
      return "unknown"sv;
    }

  }  // namespace

  coordinator_t::coordinator_t(std::unique_ptr<steps_t> steps, stream_mode_t current, send_fn_t send, options_t options):
      m_steps {std::move(steps)},
      m_send {std::move(send)},
      m_options {options},
      m_current {current} {
    m_thread = std::thread {[this] {
      run();
    }};
  }

  coordinator_t::~coordinator_t() {
    stop();
  }

  void coordinator_t::submit(const request_t &request) {
    std::lock_guard lock {m_mutex};
    if (m_stopping) {
      return;
    }

    if (request.id <= m_last_id) {
      // The client numbers its requests in order, so this is a repeat or it
      // overtook a newer one. The client would drop its answer as stale, and
      // answering it now would put the answers out of order.
      BOOST_LOG(warning) << "Ignoring stream resize request "sv << request.id << ", which is not newer than "sv << m_last_id;
      return;
    }
    m_last_id = request.id;
    m_answers.push_back({request.id});

    if (m_phase == phase_e::poisoned) {
      decide(request.id, status_e::rejected_unsupported);
      return;
    }

    if (!m_options.fps_change_supported && request.mode.fps != m_current.fps) {
      decide(request.id, status_e::rejected_unsupported);
      return;
    }

    if (m_waiting) {
      // Only the newest request is worth carrying out. The one it displaces
      // still gets its answer, in its turn.
      decide(m_waiting->id, status_e::superseded);
    }
    m_waiting = request;
    m_wake.notify_all();
  }

  void coordinator_t::reject_invalid(std::uint32_t id) {
    std::lock_guard lock {m_mutex};
    if (m_stopping || id <= m_last_id) {
      return;
    }

    m_last_id = id;
    m_answers.push_back({id});
    decide(id, status_e::rejected_invalid);
  }

  void coordinator_t::request_stop() {
    {
      std::lock_guard lock {m_mutex};
      m_stopping = true;
      m_waiting.reset();
      m_answers.clear();
    }
    m_wake.notify_all();
    m_steps->interrupt();
  }

  void coordinator_t::stop() {
    request_stop();

    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
      m_thread.join();
    }
  }

  phase_e coordinator_t::phase() const {
    std::lock_guard lock {m_mutex};
    return m_phase;
  }

  stream_mode_t coordinator_t::current_mode() const {
    std::lock_guard lock {m_mutex};
    return m_current;
  }

  void coordinator_t::run() {
    for (;;) {
      request_t request;
      {
        std::unique_lock lock {m_mutex};
        m_wake.wait(lock, [this] {
          return m_stopping || m_waiting;
        });
        if (m_stopping) {
          return;
        }

        request = *std::exchange(m_waiting, std::nullopt);
        if (request.mode == m_current) {
          // An earlier request already got the stream here.
          decide(request.id, status_e::ok, m_current);
          continue;
        }
      }

      const auto result = transact(request);

      bool poisoned = false;
      {
        std::lock_guard lock {m_mutex};
        if (m_stopping || !result) {
          return;
        }

        if (result->status == status_e::ok) {
          m_current = result->mode;
        }
        decide(result->id, result->status, result->mode);

        if (m_phase == phase_e::poisoned) {
          poisoned = true;
          if (m_waiting) {
            decide(m_waiting->id, status_e::rejected_unsupported);
            m_waiting.reset();
          }
        }
      }

      if (poisoned) {
        // The answer is already on its way, so the client learns why the
        // session ends rather than just seeing it end.
        m_steps->end_session();
      }
    }
  }

  std::optional<result_t> coordinator_t::transact(const request_t &request) {
    std::uint64_t generation;
    stream_mode_t from;
    {
      std::lock_guard lock {m_mutex};
      generation = ++m_generation;
      from = m_current;
    }

    BOOST_LOG(info) << "Stream resize "sv << request.id << " (generation "sv << generation << "): "sv
                    << from.width << 'x' << from.height << " -> "sv << request.mode.width << 'x' << request.mode.height
                    << " @ "sv << request.mode.fps << "fps"sv;

    // A session that stops between steps takes nothing further. Ending it
    // restores the display configuration and removes every display the lease
    // holds, whichever step it stopped at.
    set_phase(phase_e::preparing);
    if (!m_steps->create_display(generation, request.mode)) {
      BOOST_LOG(warning) << "Stream resize "sv << request.id << ": the new display did not come up"sv;
      return roll_back(generation, stage_e::create, request);
    }
    if (stopping()) {
      return std::nullopt;
    }

    set_phase(phase_e::switching);
    if (!m_steps->switch_topology(generation)) {
      BOOST_LOG(warning) << "Stream resize "sv << request.id << ": the display configuration was not applied"sv;
      return roll_back(generation, stage_e::topology, request);
    }
    if (stopping()) {
      return std::nullopt;
    }

    if (!m_steps->switch_video(generation, request.mode)) {
      BOOST_LOG(warning) << "Stream resize "sv << request.id << ": the encoder did not run at the new size"sv;
      return roll_back(generation, stage_e::video, request);
    }
    if (stopping()) {
      return std::nullopt;
    }

    // The new display is being streamed from here on, so nothing after this
    // point is undone.
    set_phase(phase_e::committed);
    if (!m_steps->remove_old_display(generation)) {
      BOOST_LOG(warning) << "Stream resize "sv << request.id
                         << ": the old display was not removed. The lease keeps it and removes it when the session ends."sv;
    }

    set_phase(phase_e::idle);
    return result_t {request.id, request.mode, status_e::ok};
  }

  std::optional<result_t> coordinator_t::roll_back(std::uint64_t generation, stage_e stage, const request_t &request) {
    stream_mode_t from;
    {
      std::lock_guard lock {m_mutex};
      if (m_stopping) {
        // The step failed because the session is stopping. Ending the session
        // restores the display configuration and removes every display the
        // lease holds, so there is nothing to undo here.
        return std::nullopt;
      }
      from = m_current;
    }

    set_phase(phase_e::rolling_back);

    // The desktop goes back first, because the capture can only find a
    // display that is on the desktop. The encoder is then rebuilt on the old
    // display and checked, and only once that works is the new display
    // removed. A display that never got onto the desktop only needs removing.
    bool restored = true;
    if (stage != stage_e::create) {
      restored = m_steps->restore_topology(generation) && !stopping() && m_steps->restore_video(generation, from);
    }
    restored = restored && !stopping() && m_steps->remove_new_display(generation);

    if (!restored) {
      if (stopping()) {
        return std::nullopt;
      }

      BOOST_LOG(error) << "Stream resize "sv << request.id << ": the old display could not be put back. Ending the session."sv;
      set_phase(phase_e::poisoned);
      return result_t {request.id, from, status_e::failed_session_ending};
    }

    set_phase(phase_e::idle);
    return result_t {request.id, from, status_e::failed_rolled_back};
  }

  bool coordinator_t::stopping() const {
    std::lock_guard lock {m_mutex};
    return m_stopping;
  }

  void coordinator_t::set_phase(phase_e phase) {
    std::lock_guard lock {m_mutex};
    m_phase = phase;
  }

  void coordinator_t::decide(std::uint32_t id, status_e status, std::optional<stream_mode_t> mode) {
    const auto answer = std::ranges::find_if(m_answers, [id](const answer_t &candidate) {
      return candidate.id == id;
    });
    if (answer == m_answers.end()) {
      return;
    }

    answer->status = status;
    answer->mode = mode;
    send_decided();
  }

  void coordinator_t::send_decided() {
    while (!m_answers.empty() && m_answers.front().status) {
      const auto &answer = m_answers.front();

      // Anything but OK reports what the stream is encoded at as the answer
      // leaves, which accounts for every request answered before it.
      const result_t result {answer.id, answer.mode.value_or(m_current), *answer.status};
      BOOST_LOG(info) << "Stream resize "sv << result.id << ' ' << to_string(result.status) << ": streaming "sv
                      << result.mode.width << 'x' << result.mode.height << " @ "sv << result.mode.fps << "fps"sv;
      m_send(result);

      m_answers.pop_front();
    }
  }

  std::optional<bool> await_video_ack(
    std::uint64_t generation,
    std::chrono::milliseconds timeout,
    const std::function<std::optional<video_ack_t>(std::chrono::milliseconds)> &pop
  ) {
    const auto give_up_at = std::chrono::steady_clock::now() + timeout;

    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= give_up_at) {
        return std::nullopt;
      }

      const auto ack = pop(std::chrono::ceil<std::chrono::milliseconds>(give_up_at - now));
      if (!ack) {
        return std::nullopt;
      }

      if (ack->generation == generation) {
        return ack->applied;
      }

      BOOST_LOG(debug) << "Ignoring the answer to config change "sv << ack->generation << " while waiting for "sv << generation;
    }
  }

}  // namespace stream_resize
