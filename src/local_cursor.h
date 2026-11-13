/**
 * @file src/local_cursor.h
 * @brief The mouse pointer as clients that draw it themselves see it.
 *
 * A client can ask to draw the pointer itself, at its own mouse position, so
 * the pointer moves without waiting for video. The capture code reports the
 * pointer's shape and visibility here, and the control stream sends them on to
 * those clients.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace local_cursor {

  /**
   * @brief Kinds of pointer shape, numbered as DXGI_OUTDUPL_POINTER_SHAPE_TYPE.
   */
  enum class shape_type_e : int {
    monochrome = 1,  ///< An AND mask over an XOR mask, one bit per pixel.
    color = 2,  ///< 32-bit BGRA with alpha.
    masked_color = 4,  ///< 32-bit BGR, where an alpha of 0xFF means XOR with the screen.
  };

  /**
   * @brief A pointer image as a client draws it.
   */
  struct image_t {
    int width;  ///< Width in pixels.
    int height;  ///< Height in pixels.
    int hot_x;  ///< Horizontal position of the hot spot.
    int hot_y;  ///< Vertical position of the hot spot.
    int scale;  ///< How many times the pointer was shrunk to fit, 1 if not at all.
    std::vector<std::uint8_t> bgra;  ///< Pixels as 32-bit BGRA with straight alpha, row by row.
  };

  /**
   * @brief Turn a captured pointer shape into an image a client can draw.
   *
   * Pixels that invert the screen cannot be drawn as such, so they become
   * black with a white edge, which shows on any background. A pointer with
   * more pixels than max_pixels is shrunk by a whole factor until it fits.
   *
   * @param type Kind of shape.
   * @param width Width in pixels.
   * @param height Height in pixels. For a monochrome shape this covers both masks.
   * @param pitch Bytes from one row to the next.
   * @param hot_x Horizontal position of the hot spot.
   * @param hot_y Vertical position of the hot spot.
   * @param data The shape.
   * @param max_pixels Most pixels the image may have.
   * @return The image, or nothing for a shape that is empty or does not fit its data.
   */
  std::optional<image_t> from_shape(shape_type_e type, int width, int height, int pitch, int hot_x, int hot_y, std::span<const std::uint8_t> data, int max_pixels);

  /**
   * @brief What the pointer looks like now.
   */
  struct state_t {
    std::uint64_t serial;  ///< Changes whenever anything else here does.
    bool visible;  ///< Whether the pointer is shown.
    std::shared_ptr<const image_t> image;  ///< How it looks, once known.
  };

  /**
   * @brief Report a new pointer shape.
   * @param image How the pointer looks now.
   */
  void set_image(std::shared_ptr<const image_t> image);

  /**
   * @brief Report whether the pointer is shown.
   * @param visible True when it is.
   */
  void set_visible(bool visible);

  /**
   * @brief What the pointer looks like now.
   * @return The latest reported state.
   */
  state_t state();

}  // namespace local_cursor
