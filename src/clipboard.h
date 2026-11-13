/**
 * @file src/clipboard.h
 * @brief Declarations for sharing the host clipboard with the streaming client.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace clipboard {
  /**
   * @brief What a clipboard holds, as it travels between host and client.
   */
  struct content_t {
    /**
     * @brief The kinds of content that are shared.
     */
    enum class kind_e {
      text,  ///< UTF-8 text.
      png  ///< An image, as a PNG file.
    };

    kind_e kind {};  ///< What the data is.
    std::string data;  ///< The text, or the PNG file.
  };

  /**
   * @brief The largest content either side sends or accepts.
   */
  inline constexpr std::size_t max_content_bytes {32 * 1024 * 1024};

  /**
   * @brief Whether this host can share its clipboard.
   *
   * @return True on platforms with an implementation.
   */
  [[nodiscard]] bool supported();

  /**
   * @brief A number that changes whenever the host clipboard changes.
   *
   * @return The clipboard's current sequence number.
   */
  [[nodiscard]] std::uint32_t sequence();

  /**
   * @brief What the host clipboard holds.
   *
   * Text is preferred when the clipboard offers both, since a copy that
   * carries a picture of itself, such as spreadsheet cells, is usually meant
   * as text.
   *
   * @return The text, else the image, else nothing.
   */
  [[nodiscard]] std::optional<content_t> read();

  /**
   * @brief Replace what the host clipboard holds.
   *
   * @param content What to put on the clipboard.
   * @return The sequence number the clipboard has afterwards, or nothing if it could not be written.
   */
  [[nodiscard]] std::optional<std::uint32_t> write(const content_t &content);

  /**
   * @brief The MIME type content of a kind travels as.
   *
   * @param kind The kind of content.
   * @return Its MIME type.
   */
  [[nodiscard]] std::string_view mime_type(content_t::kind_e kind);

  /**
   * @brief The kind of content a MIME type names.
   *
   * @param mime_type A Content-Type value, parameters allowed.
   * @return The kind, or nothing for a type the clipboard does not share.
   */
  [[nodiscard]] std::optional<content_t::kind_e> kind_of(std::string_view mime_type);

  /**
   * @brief A BMP file around a device-independent bitmap.
   *
   * The clipboard holds a bitmap without the file header that image decoders
   * expect, and where the pixels start depends on the header, the masks and
   * the color table in front of them.
   *
   * @param dib The bitmap as the clipboard holds it.
   * @return The BMP file, or nothing if the bitmap header is not one this understands.
   */
  [[nodiscard]] std::optional<std::string> bmp_file_from_dib(std::string_view dib);

  /**
   * @brief A device-independent bitmap from 32-bit pixels.
   *
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param bgra Top-down rows of blue, green, red and alpha bytes.
   * @return A bottom-up bitmap with a plain 32-bit header, or nothing if the pixels do not match the size.
   */
  [[nodiscard]] std::optional<std::string> dib_from_bgra(std::uint32_t width, std::uint32_t height, std::string_view bgra);
}  // namespace clipboard
