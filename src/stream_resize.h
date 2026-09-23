/**
 * @file src/stream_resize.h
 * @brief Declarations for resizing a running stream without reconnecting.
 *
 * The client asks for a new size when its window changes. The host cannot
 * change the size of the virtual display it streams, because the driver fixes
 * a display's modes when it creates it, so a resize replaces the display: a
 * new one is created at the new size, the host switches to it, and the old one
 * is removed. Make before break, so the desktop always has a display to be on.
 *
 * This file holds the part that decides what happens: one transaction at a
 * time, each step undone in turn when a later one fails, and answers to the
 * client in the order it asked. What each step does to the host is behind an
 * interface, so the decisions can be tested without a driver or an encoder.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace stream_resize {

  /**
   * @brief What a stream is encoded at.
   */
  struct stream_mode_t {
    int width {};  ///< Width in pixels.
    int height {};  ///< Height in pixels.
    int fps {};  ///< Frames per second.

    /**
     * @brief Whether two modes are the same.
     *
     * @param lhs One mode.
     * @param rhs The other.
     * @return True if they match in every field.
     */
    friend bool operator==(const stream_mode_t &lhs, const stream_mode_t &rhs) = default;
  };

  /**
   * @brief How a request ended. The values are the ones sent to the client.
   */
  enum class status_e : std::uint8_t {
    ok = 0,  ///< The stream now has the requested mode.
    superseded = 1,  ///< A newer request replaced this one before it started.
    rejected_invalid = 2,  ///< The request was malformed or out of range.
    rejected_unsupported = 3,  ///< The host cannot resize in its current configuration.
    failed_rolled_back = 4,  ///< The resize failed and the previous stream was restored.
    failed_session_ending = 5  ///< The resize failed without recovery, so the session is ending.
  };

  /**
   * @brief A request from the client.
   */
  struct request_t {
    std::uint32_t id {};  ///< Request id, increasing for every request on a connection.
    stream_mode_t mode {};  ///< What the client asks for.
  };

  /**
   * @brief An answer to the client.
   */
  struct result_t {
    std::uint32_t id {};  ///< The request this answers.
    stream_mode_t mode {};  ///< What the stream is encoded at once the answer is sent.
    status_e status {};  ///< How the request ended.
  };

  /**
   * @brief Where a transaction is.
   */
  enum class phase_e {
    idle,  ///< No transaction.
    preparing,  ///< Creating the new display. The stream still shows the old one.
    switching,  ///< Moving the desktop, the capture and the encoder to the new display.
    committed,  ///< The stream shows the new display. The old one is being removed.
    rolling_back,  ///< Putting the old display back after a step failed.
    poisoned  ///< Putting it back failed too. Nothing is known, and the session is ending.
  };

  /**
   * @brief What a resize does to the host, one step at a time.
   *
   * The coordinator calls these from one thread, one at a time, in the order
   * they are declared. Every call must return, and bound its own waits. The
   * generation passed in is the transaction's, which is what a step uses to
   * ignore late answers to an earlier one.
   */
  class steps_t {
  public:
    virtual ~steps_t() = default;

    /**
     * @brief Preparing: create a display at the new mode next to the current one.
     *
     * @param generation Transaction generation.
     * @param mode Mode the new display must offer.
     * @return True once the display exists and can be configured.
     */
    virtual bool create_display(std::uint64_t generation, const stream_mode_t &mode) = 0;

    /**
     * @brief Switching: make the new display the one the desktop is on and the capture follows.
     *
     * @param generation Transaction generation.
     * @return True if the display configuration was applied.
     */
    virtual bool switch_topology(std::uint64_t generation) = 0;

    /**
     * @brief Switching: rebuild the encoder for the new mode.
     *
     * @param generation Transaction generation.
     * @param mode Mode to encode at.
     * @return True once an encoder for the mode is running.
     */
    virtual bool switch_video(std::uint64_t generation, const stream_mode_t &mode) = 0;

    /**
     * @brief Committed: remove the old display.
     *
     * @param generation Transaction generation.
     * @return True if it was removed. A failure is not undone, because the
     *         new display is already being streamed.
     */
    virtual bool remove_old_display(std::uint64_t generation) = 0;

    /**
     * @brief Rolling back: put the desktop and the capture back on the old display.
     *
     * @param generation Transaction generation.
     * @return True if the old display configuration was applied.
     */
    virtual bool restore_topology(std::uint64_t generation) = 0;

    /**
     * @brief Rolling back: rebuild the encoder for the old mode and check it runs.
     *
     * @param generation Transaction generation.
     * @param mode Mode the stream had before the transaction.
     * @return True once an encoder for the mode is running.
     */
    virtual bool restore_video(std::uint64_t generation, const stream_mode_t &mode) = 0;

    /**
     * @brief Rolling back: remove the new display.
     *
     * @param generation Transaction generation.
     * @return True if it is gone.
     */
    virtual bool remove_new_display(std::uint64_t generation) = 0;

    /**
     * @brief Poisoned: end the session, since the host is in a state nothing vouches for.
     */
    virtual void end_session() = 0;

    /**
     * @brief Make a wait in progress give up. Called from another thread when the session stops.
     */
    virtual void interrupt() {
    }
  };

  /**
   * @brief How the coordinator treats requests it could otherwise act on.
   */
  struct options_t {
    bool fps_change_supported {false};  ///< Whether a request may change the frame rate.
  };

  /**
   * @brief Runs one session's resizes, one at a time, and answers them in order.
   *
   * Requests arrive on the control thread and are never waited on there. One
   * transaction runs at a time on the coordinator's own thread. A request that
   * arrives meanwhile waits in a single slot, and one arriving after it takes
   * the slot and the one it displaced is answered as superseded.
   *
   * Answers go out in the order the requests arrived, even when a later
   * request is decided first. The client treats an answer older than one it
   * has already seen as stale, so an answer sent out of order would hide a
   * resize that really happened.
   */
  class coordinator_t {
  public:
    /**
     * @brief Hands an answer to the client. Must not block or call back into the coordinator.
     */
    using send_fn_t = std::function<void(const result_t &)>;

    /**
     * @param steps What each step does to the host.
     * @param current What the stream is encoded at now.
     * @param send Hands answers to the client.
     * @param options How to treat requests.
     */
    coordinator_t(std::unique_ptr<steps_t> steps, stream_mode_t current, send_fn_t send, options_t options = {});
    ~coordinator_t();

    coordinator_t(const coordinator_t &) = delete;
    coordinator_t &operator=(const coordinator_t &) = delete;

    /**
     * @brief Take a request. Returns at once; the answer comes through send.
     *
     * @param request The request, already read from the wire.
     */
    void submit(const request_t &request);

    /**
     * @brief Answer a request whose id could be read but nothing else.
     *
     * @param id The request id.
     */
    void reject_invalid(std::uint32_t id);

    /**
     * @brief Stop for good: drop waiting requests and cut the running step short.
     *
     * Nothing is undone. The session is ending, and ending it restores the
     * display configuration and removes every display the lease holds. Safe
     * to call from any thread, including from a step.
     */
    void request_stop();

    /**
     * @brief request_stop(), then wait for the coordinator's thread to finish.
     */
    void stop();

    /**
     * @brief Where the current transaction is.
     *
     * @return The phase.
     */
    [[nodiscard]] phase_e phase() const;

    /**
     * @brief What the stream is encoded at.
     *
     * @return The mode as of the last finished transaction.
     */
    [[nodiscard]] stream_mode_t current_mode() const;

  private:
    /**
     * @brief One request's place in the answer order.
     */
    struct answer_t {
      std::uint32_t id {};  ///< The request.
      std::optional<status_e> status;  ///< How it ended, once decided.
      std::optional<stream_mode_t> mode;  ///< Set for OK; otherwise the current mode when sent.
    };

    /**
     * @brief Where a transaction failed, which decides how much is undone.
     */
    enum class stage_e {
      create,  ///< The new display was not ready.
      topology,  ///< The display configuration was not applied.
      video  ///< The encoder did not run at the new mode.
    };

    /**
     * @brief The coordinator's thread: take the waiting request and carry it out, until stopped.
     */
    void run();

    /**
     * @brief Carry out one request.
     *
     * @param request The request.
     * @return The answer, or nothing if the session stopped in the middle.
     */
    std::optional<result_t> transact(const request_t &request);

    /**
     * @brief Undo what a failed transaction did, most recent step first.
     *
     * @param generation Transaction generation.
     * @param stage The step that failed.
     * @param request The request being carried out.
     * @return The answer, or nothing if the session stopped in the middle.
     */
    std::optional<result_t> roll_back(std::uint64_t generation, stage_e stage, const request_t &request);

    /**
     * @brief Whether the coordinator was told to stop.
     *
     * @return True once stopping.
     */
    [[nodiscard]] bool stopping() const;

    /**
     * @brief Record where the transaction is.
     *
     * @param phase The new phase.
     */
    void set_phase(phase_e phase);

    /**
     * @brief Settle how a request ended, and send every answer that is now next in line.
     *
     * Called with m_mutex held.
     *
     * @param id The request.
     * @param status How it ended.
     * @param mode What the stream is encoded at, for OK. Other answers take the mode when they leave.
     */
    void decide(std::uint32_t id, status_e status, std::optional<stream_mode_t> mode = std::nullopt);

    /**
     * @brief Send the answers at the front of the line that are settled. Called with m_mutex held.
     */
    void send_decided();

    std::unique_ptr<steps_t> m_steps;
    send_fn_t m_send;
    options_t m_options;

    mutable std::mutex m_mutex;  ///< Guards everything below.
    std::condition_variable m_wake;
    stream_mode_t m_current;
    phase_e m_phase {phase_e::idle};
    std::uint64_t m_generation {0};
    std::uint32_t m_last_id {0};  ///< Newest request id seen. Older ones are ignored.
    std::optional<request_t> m_waiting;  ///< The one request waiting for the running one.
    bool m_stopping {false};
    std::deque<answer_t> m_answers;  ///< In the order the requests arrived.

    std::thread m_thread;
  };

  /**
   * @brief Answers waiting for the control channel, kept until they have gone out.
   *
   * An answer that fails to send stays at the front, and the ones behind it
   * wait, so the client still gets them in order. Only used by the control
   * thread.
   */
  class outbox_t {
  public:
    /**
     * @brief How long answers may keep failing to go out before the session has to end.
     *
     * A client whose answer never arrives cannot tell what the stream is, so
     * the session is ended rather than left in that state.
     */
    static constexpr std::chrono::seconds give_up_after {5};

    /**
     * @brief Queue an answer behind the ones already waiting.
     *
     * @param result The answer.
     */
    void push(const result_t &result);

    /**
     * @brief Send what is waiting, in order, stopping at the first that does not go out.
     *
     * @param send Sends one answer. Returns false if it did not go out.
     * @param now The time.
     * @return False once answers have been failing to go out for longer than give_up_after.
     */
    bool flush(const std::function<bool(const result_t &)> &send, std::chrono::steady_clock::time_point now);

    /**
     * @brief Whether nothing is waiting.
     *
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const;

  private:
    std::deque<result_t> m_waiting;
    std::optional<std::chrono::steady_clock::time_point> m_failing_since;
  };

  /**
   * @brief One answer from the video thread about a config change.
   */
  struct video_ack_t {
    std::uint64_t generation {};  ///< The change this answers.
    bool applied {};  ///< Whether an encoder for it is running.
  };

  /**
   * @brief Wait for the answer to one config change, ignoring answers to others.
   *
   * An answer to an earlier change can arrive late, after a transaction gave
   * up on it and moved on. Taking it as the answer to the current change
   * would report a mode the encoder is not running at.
   *
   * @param generation The change whose answer is wanted.
   * @param timeout How long to wait in total.
   * @param pop Waits up to the given time for the next answer. Nothing means none came.
   * @return Whether the change was applied, or nothing if no answer to it came in time.
   */
  std::optional<bool> await_video_ack(
    std::uint64_t generation,
    std::chrono::milliseconds timeout,
    const std::function<std::optional<video_ack_t>(std::chrono::milliseconds)> &pop
  );

}  // namespace stream_resize
