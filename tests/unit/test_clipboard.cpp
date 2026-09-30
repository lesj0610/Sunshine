/**
 * @file tests/unit/test_clipboard.cpp
 * @brief Test src/clipboard.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <string>

// local includes
#include <src/clipboard.h>

namespace {
  using kind_e = clipboard::content_t::kind_e;

  /**
   * @brief A little-endian field, as it sits in a bitmap header.
   */
  std::string le32(std::uint32_t value) {
    return {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF), static_cast<char>((value >> 16) & 0xFF), static_cast<char>(value >> 24)};
  }

  std::uint32_t read_le32(std::string_view data, std::size_t offset) {
    std::uint32_t value {};
    for (std::size_t i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
    }
    return value;
  }

  /**
   * @brief A bitmap header with the fields that decide where the pixels start.
   */
  std::string dib_header(std::uint32_t header_size, std::uint16_t bit_count, std::uint32_t compression, std::uint32_t colors_used) {
    // Never shorter than the fields written below, whatever size it claims
    std::string header(std::max<std::uint32_t>(header_size, 40), '\0');
    header.replace(0, 4, le32(header_size));
    header[14] = static_cast<char>(bit_count & 0xFF);
    header[15] = static_cast<char>(bit_count >> 8);
    header.replace(16, 4, le32(compression));
    header.replace(32, 4, le32(colors_used));
    return header;
  }

  /**
   * @brief Where a BMP file says its pixels start.
   */
  std::uint32_t pixels_offset(const std::optional<std::string> &file) {
    return file ? read_le32(*file, 10) : 0;
  }
}  // namespace

TEST(ClipboardMime, NamesTheTypesItShares) {
  EXPECT_EQ(clipboard::kind_of("text/plain"), kind_e::text);
  EXPECT_EQ(clipboard::kind_of("Text/Plain; charset=UTF-8"), kind_e::text);
  EXPECT_EQ(clipboard::kind_of(" image/png "), kind_e::png);
  EXPECT_EQ(clipboard::kind_of(clipboard::mime_type(kind_e::text)), kind_e::text);
  EXPECT_EQ(clipboard::kind_of(clipboard::mime_type(kind_e::png)), kind_e::png);
}

TEST(ClipboardMime, RefusesOtherTypes) {
  EXPECT_FALSE(clipboard::kind_of("text/html"));
  EXPECT_FALSE(clipboard::kind_of("image/bmp"));
  EXPECT_FALSE(clipboard::kind_of(""));
  EXPECT_FALSE(clipboard::kind_of(" ; charset=utf-8"));
}

TEST(ClipboardBitmap, PutsThePixelsRightAfterAPlainHeader) {
  const auto dib {dib_header(40, 32, 0, 0) + std::string(16, '\x7f')};
  const auto file {clipboard::bmp_file_from_dib(dib)};

  ASSERT_TRUE(file);
  EXPECT_EQ(file->substr(0, 2), "BM");
  EXPECT_EQ(read_le32(*file, 2), 14 + dib.size());
  EXPECT_EQ(pixels_offset(file), 14 + 40);
  EXPECT_EQ(file->substr(14), dib);
}

TEST(ClipboardBitmap, SkipsTheMasksAfterAPlainHeader) {
  EXPECT_EQ(pixels_offset(clipboard::bmp_file_from_dib(dib_header(40, 32, 3, 0) + std::string(12 + 16, '\0'))), 14 + 40 + 12);
  EXPECT_EQ(pixels_offset(clipboard::bmp_file_from_dib(dib_header(40, 32, 6, 0) + std::string(16 + 16, '\0'))), 14 + 40 + 16);
}

TEST(ClipboardBitmap, KeepsTheMasksOfALargerHeaderInside) {
  EXPECT_EQ(pixels_offset(clipboard::bmp_file_from_dib(dib_header(124, 32, 3, 0) + std::string(16, '\0'))), 14 + 124);
}

TEST(ClipboardBitmap, SkipsTheColorTable) {
  EXPECT_EQ(pixels_offset(clipboard::bmp_file_from_dib(dib_header(40, 8, 0, 0) + std::string(256 * 4 + 16, '\0'))), 14 + 40 + 256 * 4);
  EXPECT_EQ(pixels_offset(clipboard::bmp_file_from_dib(dib_header(40, 8, 0, 2) + std::string(2 * 4 + 16, '\0'))), 14 + 40 + 2 * 4);
}

TEST(ClipboardBitmap, RefusesABitmapItCannotPlace) {
  EXPECT_FALSE(clipboard::bmp_file_from_dib(std::string(39, '\0')));
  EXPECT_FALSE(clipboard::bmp_file_from_dib(dib_header(12, 32, 0, 0) + std::string(40, '\0')));
  EXPECT_FALSE(clipboard::bmp_file_from_dib(dib_header(40, 8, 0, 0)));
  EXPECT_FALSE(clipboard::bmp_file_from_dib(dib_header(40, 32, 0, 0xFFFFFFFF)));
}

TEST(ClipboardBitmap, BuildsABottomUpBitmapFromTopDownRows) {
  // Two rows of one pixel each: the top one blue, the bottom one red
  const std::string top {"\xff\x00\x00\xff", 4};
  const std::string bottom {"\x00\x00\xff\xff", 4};
  const auto dib {clipboard::dib_from_bgra(1, 2, top + bottom)};

  ASSERT_TRUE(dib);
  EXPECT_EQ(read_le32(*dib, 0), 40);
  EXPECT_EQ(read_le32(*dib, 4), 1);
  EXPECT_EQ(read_le32(*dib, 8), 2);
  EXPECT_EQ(static_cast<unsigned char>((*dib)[14]), 32);
  EXPECT_EQ(read_le32(*dib, 16), 0);
  EXPECT_EQ(dib->substr(40), bottom + top);
}

TEST(ClipboardBitmap, RefusesPixelsThatDoNotMatchTheSize) {
  EXPECT_FALSE(clipboard::dib_from_bgra(2, 2, std::string(12, '\0')));
  EXPECT_FALSE(clipboard::dib_from_bgra(0, 1, ""));
}

#ifndef _WIN32
TEST(Clipboard, IsNotSharedWithoutAnImplementation) {
  EXPECT_FALSE(clipboard::supported());
  EXPECT_FALSE(clipboard::read());
  EXPECT_FALSE(clipboard::write({kind_e::text, "text"}));
}
#endif
