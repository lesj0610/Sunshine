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
    already_leased  ///< Another session already holds the lease.
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
  struct resolution_t {
    /**
     * @brief How far along the display is.
     */
    enum class state_e {
      not_ready,  ///< Windows has not brought the display up, or it is missing an identity.
      mode_missing,  ///< The display is up but does not offer the requested mode.
      ready  ///< Usable.
    };

    state_e state {state_e::not_ready};
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
     * @return True if the driver accepted it.
     */
    virtual bool add(const uuid_util::uuid_t &id, const std::string &client_name, const std::string &client_uid, const mode_t &mode) = 0;

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
  };

  /**
   * @brief Build the backend for this platform.
   *
   * @return The driver backend, or nullptr where there is none.
   */
  [[nodiscard]] std::unique_ptr<backend_t> make_backend();

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
     * @param backend Driver backend. May be null, which makes every acquire fail.
     * @param device_id_lookup Translates a GDI name to the id Sunshine uses.
     * @param timeouts Waits to apply.
     */
    manager_t(std::unique_ptr<backend_t> backend, device_id_lookup_t device_id_lookup, timeouts_t timeouts = {});
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
     */
    [[nodiscard]] bool leased() const;

    /**
     * @brief The display Sunshine should be configuring and capturing.
     *
     * @return The leased display's device id, or nothing when no lease is held.
     */
    [[nodiscard]] std::optional<std::string> output_override() const;

    struct impl_t;

  private:
    // Shared rather than unique: the heartbeat thread cannot be joined, so it
    // has to be able to outlive the manager without touching freed memory.
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
