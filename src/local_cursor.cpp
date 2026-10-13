/**
 * @file src/local_cursor.cpp
 * @brief Definitions for the mouse pointer as clients that draw it themselves see it.
 */
// standard includes
#include <algorithm>
#include <mutex>

// local includes
#include "local_cursor.h"

namespace local_cursor {
  namespace {
    // Pixels as A << 24 | R << 16 | G << 8 | B
    constexpr std::uint32_t transparent = 0;
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;

    std::uint32_t read_bgra(const std::uint8_t *p) {
      return std::uint32_t {p[3]} << 24 | std::uint32_t {p[2]} << 16 | std::uint32_t {p[1]} << 8 | p[0];
    }

    /**
     * @brief Give pixels that invert the screen a colour that shows everywhere.
     *
     * They become black, and the transparent pixels next to them white.
     */
    void colour_inverted(std::vector<std::uint32_t> &pixels, const std::vector<bool> &inverts, int width, int height) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const auto i = static_cast<std::size_t>(y) * width + x;
          if (inverts[i]) {
            pixels[i] = black;
            continue;
          }
          if (pixels[i] != transparent) {
            continue;
          }

          const bool edge = (x > 0 && inverts[i - 1]) || (x + 1 < width && inverts[i + 1]) ||
                            (y > 0 && inverts[i - width]) || (y + 1 < height && inverts[i + width]);
          if (edge) {
            pixels[i] = white;
          }
        }
      }
    }

    /**
     * @brief Shrink an image by a whole factor, averaging each block of pixels.
     */
    std::vector<std::uint32_t> shrink(const std::vector<std::uint32_t> &pixels, int width, int height, int factor, int new_width, int new_height) {
      std::vector<std::uint32_t> out(static_cast<std::size_t>(new_width) * new_height);

      for (int y = 0; y < new_height; ++y) {
        for (int x = 0; x < new_width; ++x) {
          // Colours are weighted by their alpha, so transparent pixels don't darken edges
          std::uint64_t a = 0, r = 0, g = 0, b = 0, count = 0;
          for (int sy = y * factor; sy < std::min(height, (y + 1) * factor); ++sy) {
            for (int sx = x * factor; sx < std::min(width, (x + 1) * factor); ++sx) {
              const auto p = pixels[static_cast<std::size_t>(sy) * width + sx];
              const std::uint64_t pa = p >> 24;
              a += pa;
              r += (p >> 16 & 0xFF) * pa;
              g += (p >> 8 & 0xFF) * pa;
              b += (p & 0xFF) * pa;
              ++count;
            }
          }

          if (a != 0) {
            out[static_cast<std::size_t>(y) * new_width + x] =
              static_cast<std::uint32_t>(a / count) << 24 | static_cast<std::uint32_t>(r / a) << 16 | static_cast<std::uint32_t>(g / a) << 8 | static_cast<std::uint32_t>(b / a);
          }
        }
      }

      return out;
    }

    std::mutex state_mutex;
    state_t current {0, true, nullptr};
  }  // namespace

  std::optional<image_t> from_shape(shape_type_e type, int width, int height, int pitch, int hot_x, int hot_y, std::span<const std::uint8_t> data, int max_pixels) {
    using enum shape_type_e;

    const int rows = type == monochrome ? height / 2 : height;
    const int min_pitch = type == monochrome ? (width + 7) / 8 : width * 4;
    if (width <= 0 || rows <= 0 || pitch < min_pitch || max_pixels <= 0 ||
        static_cast<std::size_t>(pitch) * (type == monochrome ? rows * 2 : rows) > data.size()) {
      return std::nullopt;
    }

    const auto count = static_cast<std::size_t>(width) * rows;
    std::vector<std::uint32_t> pixels(count, transparent);
    std::vector<bool> inverts(count, false);

    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto i = static_cast<std::size_t>(y) * width + x;

        if (type == monochrome) {
          const std::uint8_t bit = 0x80 >> (x % 8);
          const bool and_bit = data[static_cast<std::size_t>(y) * pitch + x / 8] & bit;
          const bool xor_bit = data[static_cast<std::size_t>(y + rows) * pitch + x / 8] & bit;
          if (!and_bit) {
            pixels[i] = xor_bit ? white : black;
          } else {
            inverts[i] = xor_bit;
          }
        } else {
          const auto p = read_bgra(&data[static_cast<std::size_t>(y) * pitch + x * 4]);
          if (type == color) {
            pixels[i] = p;
          } else if ((p >> 24) == 0) {
            // A masked colour pixel replaces the screen with its colour
            pixels[i] = black | p;
          } else {
            // or XORs the screen with it, which leaves the screen alone for black
            inverts[i] = (p & 0xFFFFFF) != 0;
          }
        }
      }
    }

    colour_inverted(pixels, inverts, width, rows);

    int factor = 1;
    int new_width = width;
    int new_height = rows;
    while (static_cast<std::int64_t>(new_width) * new_height > max_pixels) {
      ++factor;
      new_width = (width + factor - 1) / factor;
      new_height = (rows + factor - 1) / factor;
    }
    if (factor > 1) {
      pixels = shrink(pixels, width, rows, factor, new_width, new_height);
    }

    image_t image {
      .width = new_width,
      .height = new_height,
      .hot_x = std::clamp(hot_x / factor, 0, new_width - 1),
      .hot_y = std::clamp(hot_y / factor, 0, new_height - 1),
      .scale = factor,
      .bgra = std::vector<std::uint8_t>(pixels.size() * 4),
    };
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      image.bgra[i * 4] = static_cast<std::uint8_t>(pixels[i]);
      image.bgra[i * 4 + 1] = static_cast<std::uint8_t>(pixels[i] >> 8);
      image.bgra[i * 4 + 2] = static_cast<std::uint8_t>(pixels[i] >> 16);
      image.bgra[i * 4 + 3] = static_cast<std::uint8_t>(pixels[i] >> 24);
    }
    return image;
  }

  void set_image(std::shared_ptr<const image_t> image) {
    std::scoped_lock lock {state_mutex};
    current.image = std::move(image);
    ++current.serial;
  }

  void set_visible(bool visible) {
    std::scoped_lock lock {state_mutex};
    if (current.visible != visible) {
      current.visible = visible;
      ++current.serial;
    }
  }

  state_t state() {
    std::scoped_lock lock {state_mutex};
    return current;
  }

}  // namespace local_cursor
