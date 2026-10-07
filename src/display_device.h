/**
 * @file src/display_device.h
 * @brief Declarations for display device handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// lib includes
#include <display_device/display_power_interface.h>
#include <display_device/types.h>

// forward declarations
namespace platf {
  class deinit_t;
}

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace display_device {
  /**
   * @brief Initialize the implementation and perform the initial state recovery (if needed).
   * @param persistence_filepath File location for reading/saving persistent state.
   * @param video_config User's video related configuration.
   * @returns A deinit_t instance that performs cleanup when destroyed.
   *
   * @examples
   * const config::video_t &video_config { config::video };
   * const auto init_guard { init("/my/persitence/file.state", video_config) };
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config);

  /**
   * @brief Map the output name to a specific display.
   * @param output_name The user-configurable output name.
   * @returns Mapped display name or empty string if the output name could not be mapped.
   *
   * @examples
   * const auto mapped_name_config { map_output_name(config::video.output_name) };
   * const auto mapped_name_custom { map_output_name("{some-device-id}") };
   * @examples_end
   */
  [[nodiscard]] std::string map_output_name(const std::string &output_name);

  /**
   * @brief Find the id a display with the given platform name is known by.
   *
   * The inverse of map_output_name(). The driver and the OS deal in platform
   * display names, while the rest of Sunshine addresses displays by a stable
   * id, so a newly created display has to be matched up through the device
   * list before anything can be pointed at it.
   *
   * @param display_name Platform-specific display name.
   * @return The device id, or an empty string if no device carries that name.
   *
   * @examples
   * const auto device_id { device_id_for_display_name("\\\\.\\DISPLAY1") };
   * @examples_end
   */
  [[nodiscard]] std::string device_id_for_display_name(const std::string &display_name);

  /**
   * @brief The display Sunshine should be configuring and capturing right now.
   *
   * Normally the configured one. While a session holds a virtual display, that
   * display instead. Every reader of the configured output goes through here,
   * so the two never disagree about which display a session is using.
   *
   * @param video_config Video configuration to fall back to.
   * @return A device id, which map_output_name() turns into a platform name.
   *
   * @examples
   * const auto id { active_output_id(config::video) };
   * @examples_end
   */
  [[nodiscard]] std::string active_output_id(const config::video_t &video_config);

  /**
   * @brief Take the virtual display a session needs, if it is configured to have one.
   *
   * Must run before the display is configured and before encoders are probed,
   * so that both act on the display the session will actually stream.
   *
   * Deliberately all-or-nothing. A host told to stream a virtual display must
   * not quietly stream a monitor instead, because the configuration that
   * follows would then change that monitor's resolution, which is the outcome
   * the option exists to avoid. The caller refuses the session on failure.
   *
   * @param session Launch session, for the mode the client asked for.
   * @return Nothing on success, or why the session cannot be served.
   *
   * @examples
   * if (const auto reason { prepare_virtual_display(*launch_session) }) {
   *   // refuse the session, quoting *reason
   * }
   * @examples_end
   */
  [[nodiscard]] std::optional<std::string> prepare_virtual_display(const rtsp_stream::launch_session_t &session);

  /**
   * @brief Ask the platform to wake displays before detection or capture.
   * @param display_name Platform capture selector.
   * @param timeout Maximum time to wait for platform-specific wake detection.
   * @returns True if the display was already available or the wake request succeeded.
   */
  [[nodiscard]] bool wake_display(const std::string &display_name, std::chrono::milliseconds timeout);

  /**
   * @brief Keep displays awake until the returned guard is destroyed.
   * @param reason Short human-readable reason for the power assertion.
   * @returns A guard owning the platform assertion, or nullptr if unsupported or unavailable.
   */
  [[nodiscard]] std::unique_ptr<DisplayPowerGuardInterface> keep_display_awake(const std::string &reason);

  /**
   * @brief Configure the display device based on the user configuration and the session information.
   * @note This is a convenience method for calling similar method of a different signature.
   *
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return True once the configuration has taken, false if it failed or did
   *         not settle in time.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * configure_display(video_config, *launch_session);
   * @examples_end
   */
  [[nodiscard]] bool configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Configure the display device using the provided configuration.
   *
   * In some cases configuring display can fail due to transient issues and
   * we will keep trying every 5 seconds, even if the stream has already started as there was
   * no possibility to apply settings before the stream start.
   *
   * The caller is told whether it took, because some of them have to know
   * before they go on to capture the display. A transient failure keeps being
   * retried in the background either way, so that users can do something
   * about it once they are connected rather than being kept out entirely.
   *
   * @param config Configuration for the display.
   * @return True once the configuration has taken, false if it failed or did
   *         not settle in time.
   *
   * @examples
   * const SingleDisplayConfiguration valid_config { };
   * configure_display(valid_config);
   * @examples_end
   */
  [[nodiscard]] bool configure_display(const SingleDisplayConfiguration &config);

  /**
   * @brief Revert the display configuration and restore the previous state.
   *
   * In case the state could not be restored, by default it will be retried again in 5 seconds
   * (repeating indefinitely until success or until persistence is reset).
   *
   * @examples
   * revert_configuration();
   * @examples_end
   */
  void revert_configuration();

  /**
   * @brief Reset persisted display state and the captured initial state.
   *
   * This is normally used to get out of the "broken" state where the algorithm wants
   * to restore the initial display state, but it is no longer possible.
   *
   * This could happen if the display is no longer available or the hardware was changed
   * and the device ids no longer match.
   *
   * The user then accepts that Sunshine is not able to restore the state and "agrees" to
   * do it manually.
   *
   * @return True if persistence was reset, false otherwise.
   * @note Whether the function succeeds or fails, any of the scheduled "retries" from
   *       other methods will be stopped to not interfere with the user actions.
   *
   * @examples
   * const auto result = reset_persistence();
   * @examples_end
   */
  [[nodiscard]] bool reset_persistence();

  /**
   * @brief Enumerate the available devices.
   * @return A list of devices.
   *
   * @examples
   * const auto devices = enumerate_devices();
   * @examples_end
   */
  [[nodiscard]] EnumeratedDeviceList enumerate_devices();

  /**
   * @brief A tag structure indicating that configuration parsing has failed.
   */
  struct failed_to_parse_tag_t {};

  /**
   * @brief A tag structure indicating that configuration is disabled.
   */
  struct configuration_disabled_tag_t {};

  /**
   * @brief Parse the user configuration and the session information.
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return Parsed single display configuration or
   *         a tag indicating that the parsing has failed or
   *         a tag indicating that the user does not want to perform any configuration.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * const auto config { parse_configuration(video_config, *launch_session) };
   * if (const auto *parsed_config { std::get_if<SingleDisplayConfiguration>(&result) }; parsed_config) {
   *    configure_display(*config);
   * }
   * @examples_end
   */
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);
}  // namespace display_device
