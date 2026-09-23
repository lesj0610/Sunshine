/**
 * @file src/virtual_display.h
 * @brief Declarations for streaming a display created for the session.
 *
 * Matching the client's resolution by changing a monitor's mode rearranges
 * everything on the host desktop, and limits the client to modes that monitor
 * already has. A display created for the session has neither limit.
 *
 * The lifetime is a lease. Nothing streams a virtual display without holding
 * one, and releasing it puts the host back the way it was. The lease is
 * deliberately all-or-nothing: a host configured to stream a virtual display
 * must never quietly fall back to a real monitor, because the fallback would
 * then have its resolution changed, which is the outcome the option exists to
 * avoid.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// local includes
#include "uuid.h"

namespace rtsp_stream {
  struct launch_session_t;
}  // namespace rtsp_stream

namespace virtual_display {

  /**
   * @brief Why a lease could not be taken.
   */
  enum class error_e {
    driver_unavailable,  ///< The driver is not installed, or its interface would not open.
    protocol_mismatch,  ///< The driver speaks a protocol this build cannot use.
    watchdog_unavailable,  ///< The driver would not report a watchdog, so a crash would strand displays.
    heartbeat_failed,  ///< The driver stopped answering before the display was created.
    create_failed,  ///< The driver refused to create the display.
    not_ready,  ///< Windows did not bring the display up in time.
    mode_unavailable,  ///< The display came up but does not offer the requested mode.
    already_leased,  ///< Another session already holds the lease.
    busy,  ///< A driver call has not come back, so nothing may be asked of it yet.
    not_leased  ///< A replacement was asked for, but no session holds the lease.
  };

  /**
   * @brief What the lease is doing.
   *
   * Reserved under one lock at the start of an acquire, so two requests
   * arriving together cannot both decide the lease is free.
   */
  enum class state_e {
    idle,  ///< No display, and a session may take one.
    provisioning,  ///< A session is in the middle of taking one.
    active,  ///< A session holds a display.
    reverting,  ///< The display is being given back.
    poisoned,  ///< A driver call never came back; nothing may be asked of the driver.
    recovering  ///< One caller is finishing what the unanswered call left behind.
  };

  /**
   * @brief Human-readable reason, for logs and HTTP replies.
   *
   * @param error Error to describe.
   * @return A short sentence naming the cause.
   */
  [[nodiscard]] std::string to_string(error_e error);

  /**
   * @brief A display mode.
   */
  struct mode_t {
    int width {};  ///< Width in pixels.
    int height {};  ///< Height in pixels.
    int refresh_rate_millihz {};  ///< Refresh rate in thousandths of a hertz, which is the unit the driver uses.

    /**
     * @brief Whether two modes ask for the same thing.
     *
     * @param lhs One mode.
     * @param rhs The other.
     * @return True if they match in every field.
     */
    friend bool operator==(const mode_t &lhs, const mode_t &rhs) = default;
  };

  /**
   * @brief A display the driver created and Windows has brought up.
   */
  struct display_t {
    std::string device_id;  ///< Stable id, which is how the rest of Sunshine addresses a display.
    std::string gdi_name;  ///< `\\.\DISPLAY1` style name, for logs.
  };

  /**
   * @brief Translate a GDI display name into the id Sunshine addresses it by.
   *
   * The driver and Windows deal in `\\.\DISPLAY1` names, while the rest of
   * Sunshine uses the stable ids that libdisplaydevice derives from the
   * monitor's instance id and EDID. The two are matched up through the device
   * list, which is the display device layer's business rather than the
   * driver's, so the lease is handed the translation instead of doing it.
   *
   * Returning an empty string means the display is not in the list yet, which
   * is treated as "not ready" rather than as a failure.
   */
  using device_id_lookup_t = std::function<std::string(const std::string &gdi_name)>;

  /**
   * @brief What a readiness check found.
   */
  /**
   * @brief How an attempt to create a display turned out.
   */
  enum class creation_e {
    not_created,  ///< The driver refused. Nothing exists.
    created,  ///< The display exists and the driver described it.
    created_unverifiable  ///< The driver accepted it but did not describe it, so it may well exist.
  };

  /**
   * @brief What a readiness check found out about a display.
   */
  struct resolution_t {
    /**
     * @brief How far along the display is.
     */
    enum class readiness_e {
      not_ready,  ///< Windows has not brought the display up, or it is missing an identity.
      mode_missing,  ///< The display is up but does not offer the requested mode.
      ready  ///< Usable.
    };

    readiness_e state {readiness_e::not_ready};  ///< How far along the display is.
    display_t display {};  ///< Meaningful only when state is ready.
  };

  /**
   * @brief Everything the lease needs from the driver and from the OS.
   *
   * One interface so the lease can be tested without a driver. Every call is
   * expected to return rather than block forever; the manager bounds them
   * anyway, since a driver that stops answering must not take an HTTP thread
   * with it.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    /**
     * @brief Open the driver's device interface.
     * @return True if a handle was obtained.
     */
    virtual bool open() = 0;

    /**
     * @brief Close the driver handle. Safe to call when not open.
     */
    virtual void close() = 0;

    /**
     * @brief Whether the driver's protocol version is one this build can use.
     * @return True if the versions are compatible.
     */
    virtual bool protocol_supported() = 0;

    /**
     * @brief How long the driver waits before dropping displays of a silent client.
     *
     * A driver that reports no timeout would keep displays after a crash, so
     * the lease refuses to create one rather than risk stranding it.
     *
     * @return The timeout, or nothing if the driver would not say.
     */
    virtual std::optional<std::chrono::seconds> watchdog_timeout() = 0;

    /**
     * @brief Tell the driver this process is still here.
     * @return True if the driver answered.
     */
    virtual bool ping() = 0;

    /**
     * @brief Ask the driver to render on a particular adapter.
     *
     * Best effort. Capturing from an adapter other than the rendering one
     * costs a copy across the bus but still works.
     *
     * @param adapter_name Adapter description string, as configured.
     */
    virtual void set_render_adapter(const std::string &adapter_name) = 0;

    /**
     * @brief Create a display.
     *
     * @param id Identifies the display for later removal.
     * @param client_name Client name, stamped into the monitor name.
     * @param client_uid Client identifier, stamped into the monitor serial.
     * @param mode Mode the display should offer.
     * @return Whether a display was created, and whether the driver said
     *         enough about it to find it again. An answer that cannot be
     *         trusted still means a display may exist, so it has to be
     *         removable rather than forgotten.
     */
    virtual creation_e add(const uuid_util::uuid_t &id, const std::string &client_name, const std::string &client_uid, const mode_t &mode) = 0;

    /**
     * @brief Remove a display this process created.
     *
     * @param id The identifier it was created with.
     * @return True if the driver removed it.
     */
    virtual bool remove(const uuid_util::uuid_t &id) = 0;

    /**
     * @brief Look up a display the driver created, once Windows has brought it up.
     *
     * Reports whether the display is usable, which means an active path, a GDI
     * name, a device id, and the requested mode among those enumerated. A
     * display that is up but offers a different mode would stream at the wrong
     * size, so it is called out separately from one that is simply not up yet.
     *
     * @param id The identifier the display was created with.
     * @param mode Mode that must be among those the display offers.
     * @return What state the display is in, and the display itself once ready.
     */
    virtual resolution_t resolve(const uuid_util::uuid_t &id, const mode_t &mode) = 0;

    /**
     * @brief Look up the id Sunshine addresses a display by, whether or not it is on the desktop.
     *
     * A display created to replace another mid-session can come up off the
     * desktop, because Windows restores whatever arrangement it last saw for
     * the same set of monitors. The display configuration puts it on the
     * desktop, and all that needs is the id.
     *
     * @param id The identifier the display was created with.
     * @return The device id, or an empty string while Windows does not know the display yet.
     */
    virtual std::string device_id(const uuid_util::uuid_t &id) = 0;
  };

  /**
   * @brief Build the backend for this platform.
   *
   * @return The driver backend, or nullptr where there is none.
   */
  [[nodiscard]] std::unique_ptr<backend_t> make_backend();

  /**
   * @brief Makes a backend on demand.
   *
   * A driver that stopped answering is thrown away rather than reused, so the
   * lease needs to be able to ask for a new one rather than being handed a
   * single instance up front.
   */
  using backend_factory_t = std::function<std::unique_ptr<backend_t>()>;

  /**
   * @brief How long the lease waits for things that are not instant.
   */
  struct timeouts_t {
    std::chrono::milliseconds readiness {5000};  ///< For Windows to bring the display up.
    std::chrono::milliseconds readiness_poll {100};  ///< Between readiness checks.
    std::chrono::milliseconds call {5000};  ///< For any one driver call to return.
  };

  /**
   * @brief Owns the virtual display for as long as a session is using it.
   *
   * Only one lease exists at a time. Sunshine addresses the display to capture
   * through a single global, so a second session cannot be given a display of
   * its own, and is refused rather than quietly handed the first session's.
   */
  class manager_t {
  public:
    /**
     * @param make Makes the driver backend. May return null, or be null
     *             itself, which makes every acquire fail.
     * @param device_id_lookup Translates a platform display name to the id Sunshine uses.
     * @param timeouts Waits to apply.
     */
    manager_t(backend_factory_t make, device_id_lookup_t device_id_lookup, timeouts_t timeouts = {});
    ~manager_t();

    manager_t(const manager_t &) = delete;
    manager_t &operator=(const manager_t &) = delete;

    /**
     * @brief Create the display for a session and take the lease.
     *
     * On success the display is up, offers the requested mode, and
     * output_override() names it. On failure nothing was left behind and the
     * caller must refuse the session rather than stream a monitor.
     *
     * @param client_name Client name, for the monitor name.
     * @param client_uid Client identifier, for the monitor serial.
     * @param mode Mode the client asked for.
     * @param adapter_name Adapter to render on, empty to leave it to the driver.
     * @return The display, or why it could not be had.
     *
     * @examples
     * const auto result = manager.acquire("Living room", "uid", {1920, 1080, 60000}, "");
     * @examples_end
     */
    [[nodiscard]] std::variant<display_t, error_e> acquire(
      const std::string &client_name,
      const std::string &client_uid,
      const mode_t &mode,
      const std::string &adapter_name
    );

    /**
     * @brief Give up the lease and remove the display.
     *
     * Does nothing if no lease is held, so every teardown path can call it
     * without first working out whether there is anything to undo.
     */
    void release();

    /**
     * @brief Whether a session currently holds the lease.
     *
     * @return True while a session has a display.
     */
    [[nodiscard]] bool leased() const;

    /**
     * @brief What the lease is doing.
     *
     * @return The current state.
     */
    [[nodiscard]] state_e state() const;

    /**
     * @brief Which lease is current.
     *
     * Work that outlives a lease, such as a restore that had to be retried,
     * uses this to check it is still acting on the lease it started for.
     *
     * @return A number that changes with every lease taken.
     */
    [[nodiscard]] std::uint64_t generation() const;

    /**
     * @brief Give up the lease, but only if it is still the expected one.
     *
     * @param generation The lease the caller means to end.
     * @return True if that lease was the current one and has been given up.
     */
    bool release_generation(std::uint64_t generation);

    /**
     * @brief Say what to do when the driver stops answering mid-session.
     *
     * The display is gone at that point, so the session streaming it cannot
     * continue, and the host is left configured for a display that no longer
     * exists. Putting that right means stopping the stream and reverting,
     * which is not this class's business, so it is handed out instead.
     *
     * The handler runs on its own thread and must return before a new lease
     * can be taken.
     *
     * @param handler What to run. Replaces any previous handler.
     */
    void set_fault_handler(std::function<void()> handler);

    /**
     * @brief The display Sunshine should be configuring and capturing.
     *
     * @return The leased display's device id, or nothing when no lease is held.
     */
    [[nodiscard]] std::optional<std::string> output_override() const;

    /**
     * @brief Create a display to replace the leased one, for a resize.
     *
     * The driver fixes a display's modes when it creates it, so a new size
     * needs a new display. The leased display is left as it is, and
     * output_override() keeps naming it until use_replacement() says
     * otherwise, so the stream carries on while the new one comes up.
     *
     * The new display takes the other of the lease's two identities, so a
     * session that resizes many times only ever shows Windows two monitors.
     * It only has to be known to Windows, not on the desktop: the display
     * configuration puts it there.
     *
     * The replacement is owed removal from the moment it is asked for, so
     * release() removes it even if this has not returned.
     *
     * @param mode Mode the new display should offer.
     * @return The new display, whose GDI name may still be empty, or why it could not be had.
     */
    [[nodiscard]] std::variant<display_t, error_e> prepare_replacement(const mode_t &mode);

    /**
     * @brief Choose which display output_override() names while a replacement exists.
     *
     * @param use True for the replacement, false for the leased display.
     */
    void use_replacement(bool use);

    /**
     * @brief Remove the old display, and make the replacement the leased one.
     *
     * The replacement is streamed from here on whatever happens. If the old
     * display will not go, the lease keeps its identifier, removes it on
     * release, and refuses another replacement until it is gone.
     *
     * @return True if the old display was removed.
     */
    bool commit_replacement();

    /**
     * @brief Remove the replacement, and keep the leased display.
     *
     * @return True if the replacement is gone, or never existed.
     */
    bool abandon_replacement();

    struct impl_t;

  private:
    /**
     * @brief Shared by release() and release_generation().
     *
     * @param expected_generation The lease to end, or nothing for whichever is current.
     * @return False only when the lease had already moved on.
     */
    bool release_impl(std::optional<std::uint64_t> expected_generation);

    // Shared rather than unique: the heartbeat thread cannot be joined, so it
    // has to be able to outlive the manager without touching freed memory.
    std::shared_ptr<impl_t> m_impl;
  };

  /**
   * @brief Restores the display configuration, then gives the display back.
   *
   * Removing the virtual display before the saved configuration is restored
   * leaves the restore working against a display that is no longer there, so
   * the two are ordered here and nowhere else.
   *
   * There is at most one restore in progress. A second caller joins the one
   * already running rather than replacing it, because the display stack only
   * holds one retry at a time and replacing it would leave nothing to give
   * the display back.
   *
   * Everything it needs is handed in, so the ordering can be tested without a
   * display stack.
   */
  class restore_transaction_t {
  public:
    /// One attempt at restoring. Nothing means the API was busy and it is
    /// worth another go; false means it failed and is also worth another go.
    using attempt_fn_t = std::function<std::optional<bool>()>;

    /**
     * @brief Start retrying, handing each attempt back to the transaction.
     *
     * The retry runs inside the display stack's own scheduler, which holds
     * its lock while it does. Asking that scheduler for anything from in
     * there would deadlock, so the attempt is handed in instead, already
     * bound to the interface the scheduler supplied.
     *
     * The callback returns true when the transaction is finished and the
     * retries should stop.
     */
    using start_retries_fn_t = std::function<bool(std::function<bool(attempt_fn_t)>)>;

    /// Give the display back, if the lease is still the expected one.
    using release_fn_t = std::function<bool(std::uint64_t)>;

    /**
     * @param attempt One try at restoring, from a caller holding no display stack locks.
     * @param start_retries Starts retrying inside the display stack, handing each attempt back.
     * @param release Gives the display back, if the lease is still the expected one.
     */
    restore_transaction_t(attempt_fn_t attempt, start_retries_fn_t start_retries, release_fn_t release);
    ~restore_transaction_t();

    restore_transaction_t(const restore_transaction_t &) = delete;
    restore_transaction_t &operator=(const restore_transaction_t &) = delete;

    /**
     * @brief Restore for a lease and wait for it, joining one already running.
     *
     * @param generation The lease this is being done for.
     * @param timeout How long to wait for a final answer.
     * @return True once restored. False means it is still being retried, and
     *         the display stays where it is until it succeeds.
     */
    bool run(std::uint64_t generation, std::chrono::milliseconds timeout);

    /**
     * @brief Whether a restore is still being retried.
     *
     * @return True while one is outstanding.
     */
    [[nodiscard]] bool pending() const;

    /**
     * @brief Nothing can retry any more, so stop expecting one.
     *
     * Used when the display stack is torn down or replaced. One last restore
     * is attempted, and the display is given back either way, since leaving a
     * lease held by a transaction that can no longer finish would keep every
     * later session out.
     */
    void abandon();

  private:
    struct impl_t;
    std::shared_ptr<impl_t> m_impl;
  };

  /**
   * @brief The process-wide manager.
   *
   * @return The manager every caller shares.
   */
  [[nodiscard]] manager_t &manager();

  /**
   * @brief Whether the configuration asks for a virtual display.
   *
   * @return True if a session should be given one.
   */
  [[nodiscard]] bool enabled();

  /**
   * @brief The mode a session is asking for.
   *
   * @param session Launch session.
   * @return The requested mode, with the refresh rate in the driver's units.
   */
  [[nodiscard]] mode_t requested_mode(const rtsp_stream::launch_session_t &session);

}  // namespace virtual_display
