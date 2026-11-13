/**
 * @file src/clipboard.cpp
 * @brief Definitions for sharing the host clipboard that do not depend on the platform.
 */
// header include
#include "clipboard.h"

// standard includes
#include <algorithm>
#include <cctype>

namespace clipboard {
  namespace {
    constexpr std::uint32_t bitmap_info_header_size {40};  ///< Size of a BITMAPINFOHEADER.
    constexpr std::uint32_t bitmap_file_header_size {14};  ///< Size of a BITMAPFILEHEADER.
    constexpr std::uint32_t bi_bitfields {3};  ///< Masks follow a BITMAPINFOHEADER.
    constexpr std::uint32_t bi_alphabitfields {6};  ///< Masks with alpha follow a BITMAPINFOHEADER.

    std::uint32_t read_u32(std::string_view data, std::size_t offset) {
      std::uint32_t value {};
      for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
      }
      return value;
    }

    std::uint16_t read_u16(std::string_view data, std::size_t offset) {
      return static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset]) | (static_cast<unsigned char>(data[offset + 1]) << 8));
    }

    void append_u32(std::string &data, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        data.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
      }
    }

    void append_u16(std::string &data, std::uint16_t value) {
      data.push_back(static_cast<char>(value & 0xFF));
      data.push_back(static_cast<char>(value >> 8));
    }

    std::string lowercase(std::string_view text) {
      std::string lowered {text};
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return lowered;
    }
  }  // namespace

  std::string_view mime_type(content_t::kind_e kind) {
    switch (kind) {
      case content_t::kind_e::text:
        return "text/plain; charset=utf-8";
      case content_t::kind_e::png:
        return "image/png";
    }
    return {};
  }

  std::optional<content_t::kind_e> kind_of(std::string_view mime_type) {
    const auto type {lowercase(mime_type.substr(0, mime_type.find(';')))};
    const auto first {type.find_first_not_of(" \t")};
    if (first == std::string::npos) {
      return std::nullopt;
    }
    const auto trimmed {std::string_view {type}.substr(first, type.find_last_not_of(" \t") + 1 - first)};
    if (trimmed == "text/plain") {
      return content_t::kind_e::text;
    }
    if (trimmed == "image/png") {
      return content_t::kind_e::png;
    }
    return std::nullopt;
  }

  std::optional<std::string> bmp_file_from_dib(std::string_view dib) {
    if (dib.size() < bitmap_info_header_size) {
      return std::nullopt;
    }

    const auto header_size {read_u32(dib, 0)};
    if (header_size < bitmap_info_header_size || header_size > dib.size()) {
      return std::nullopt;
    }

    const auto bit_count {read_u16(dib, 14)};
    const auto compression {read_u32(dib, 16)};
    const auto colors_used {read_u32(dib, 32)};

    // Only a BITMAPINFOHEADER keeps its masks outside the header
    std::uint32_t masks_size {};
    if (header_size == bitmap_info_header_size) {
      if (compression == bi_bitfields) {
        masks_size = 12;
      } else if (compression == bi_alphabitfields) {
        masks_size = 16;
      }
    }

    const std::uint64_t colors {colors_used != 0 ? colors_used : (bit_count <= 8 ? (1u << bit_count) : 0u)};
    const std::uint64_t pixels_offset {std::uint64_t {header_size} + masks_size + colors * 4};
    if (colors > 65536 || pixels_offset > dib.size()) {
      return std::nullopt;
    }

    std::string file;
    file.reserve(bitmap_file_header_size + dib.size());
    file += "BM";
    append_u32(file, static_cast<std::uint32_t>(bitmap_file_header_size + dib.size()));
    append_u32(file, 0);
    append_u32(file, static_cast<std::uint32_t>(bitmap_file_header_size + pixels_offset));
    file += dib;
    return file;
  }

  std::optional<std::string> dib_from_bgra(std::uint32_t width, std::uint32_t height, std::string_view bgra) {
    const std::uint64_t row_size {std::uint64_t {width} * 4};
    if (width == 0 || height == 0 || row_size * height != bgra.size()) {
      return std::nullopt;
    }

    std::string dib;
    dib.reserve(bitmap_info_header_size + bgra.size());
    append_u32(dib, bitmap_info_header_size);
    append_u32(dib, width);
    append_u32(dib, height);  // Positive: the rows run bottom-up
    append_u16(dib, 1);  // Planes
    append_u16(dib, 32);  // Bits per pixel
    append_u32(dib, 0);  // BI_RGB
    append_u32(dib, static_cast<std::uint32_t>(bgra.size()));
    append_u32(dib, 0);  // Horizontal resolution
    append_u32(dib, 0);  // Vertical resolution
    append_u32(dib, 0);  // Colors used
    append_u32(dib, 0);  // Colors important

    for (std::uint32_t row = height; row-- > 0;) {
      dib += bgra.substr(row * row_size, row_size);
    }
    return dib;
  }

#ifndef _WIN32
  bool supported() {
    return false;
  }

  std::uint32_t sequence() {
    return 0;
  }

  std::optional<content_t> read() {
    return std::nullopt;
  }

  std::optional<std::uint32_t> write(const content_t &) {
    return std::nullopt;
  }
#endif
}  // namespace clipboard
