/**
 * @file tests/unit/test_local_cursor.cpp
 * @brief Test turning captured pointer shapes into images clients draw.
 */
#include "../tests_common.h"

#include <src/local_cursor.h>

using local_cursor::shape_type_e;

namespace {

  /**
   * @brief One pixel of an image as A << 24 | R << 16 | G << 8 | B.
   */
  std::uint32_t pixel(const local_cursor::image_t &image, int x, int y) {
    const auto *p = &image.bgra[(static_cast<std::size_t>(y) * image.width + x) * 4];
    return std::uint32_t {p[3]} << 24 | std::uint32_t {p[2]} << 16 | std::uint32_t {p[1]} << 8 | p[0];
  }

  constexpr std::uint32_t TRANSPARENT = 0;
  constexpr std::uint32_t BLACK = 0xFF000000;
  constexpr std::uint32_t WHITE = 0xFFFFFFFF;
}  // namespace

TEST(LocalCursor, MonochromeMasksAndInvertedPixels) {
  // One row of 8 pixels: AND mask, then XOR mask. Pixel by pixel, black,
  // white, transparent, inverted, and transparent pixels after that.
  const std::vector<std::uint8_t> data {0b00111111, 0b01010000};

  const auto image = local_cursor::from_shape(shape_type_e::monochrome, 8, 2, 1, 3, 0, data, 100);
  ASSERT_TRUE(image);
  EXPECT_EQ(image->width, 8);
  EXPECT_EQ(image->height, 1);
  EXPECT_EQ(image->hot_x, 3);
  EXPECT_EQ(image->scale, 1);
  EXPECT_EQ(pixel(*image, 0, 0), BLACK);
  EXPECT_EQ(pixel(*image, 1, 0), WHITE);
  // The inverted pixel turns black, edged in white where it meets transparency
  EXPECT_EQ(pixel(*image, 2, 0), WHITE);
  EXPECT_EQ(pixel(*image, 3, 0), BLACK);
  EXPECT_EQ(pixel(*image, 4, 0), WHITE);
  EXPECT_EQ(pixel(*image, 5, 0), TRANSPARENT);
}

TEST(LocalCursor, ColorAndMaskedColorPixels) {
  // Two pixels a row with a padded pitch, and a second row
  const std::vector<std::uint8_t> color {
    0x10, 0x20, 0x30, 0x80, 0x01, 0x02, 0x03, 0x00, 0xEE, 0xEE,
    0x40, 0x50, 0x60, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xEE,
  };
  const auto image = local_cursor::from_shape(shape_type_e::color, 2, 2, 10, 1, 1, color, 100);
  ASSERT_TRUE(image);
  EXPECT_EQ(pixel(*image, 0, 0), 0x80302010u);
  EXPECT_EQ(pixel(*image, 1, 0), 0x00030201u);
  EXPECT_EQ(pixel(*image, 0, 1), 0xFF605040u);

  // A masked colour pixel with no alpha replaces the screen, and one with
  // full alpha XORs it: with black that does nothing, with a colour it inverts
  const std::vector<std::uint8_t> masked {0x10, 0x20, 0x30, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto masked_image = local_cursor::from_shape(shape_type_e::masked_color, 3, 1, 12, 0, 0, masked, 100);
  ASSERT_TRUE(masked_image);
  EXPECT_EQ(pixel(*masked_image, 0, 0), 0xFF302010u);
  EXPECT_EQ(pixel(*masked_image, 1, 0), WHITE);
  EXPECT_EQ(pixel(*masked_image, 2, 0), BLACK);
}

TEST(LocalCursor, ShrinksLargePointersByAWholeFactor) {
  // 100 x 100 is over a limit of 8100 pixels, and 50 x 50 is not
  std::vector<std::uint8_t> data(100 * 100 * 4, 0);
  // One opaque white pixel in the top left block of four
  data[0] = data[1] = data[2] = data[3] = 0xFF;

  const auto image = local_cursor::from_shape(shape_type_e::color, 100, 100, 400, 99, 51, data, 8100);
  ASSERT_TRUE(image);
  EXPECT_EQ(image->width, 50);
  EXPECT_EQ(image->height, 50);
  EXPECT_EQ(image->scale, 2);
  EXPECT_EQ(image->hot_x, 49);
  EXPECT_EQ(image->hot_y, 25);
  // A quarter covered, and still white rather than darkened by the transparent pixels
  EXPECT_EQ(pixel(*image, 0, 0), 0x3FFFFFFFu);
  EXPECT_EQ(pixel(*image, 1, 0), TRANSPARENT);
}

TEST(LocalCursor, RejectsShapesThatDoNotFitTheirData) {
  const std::vector<std::uint8_t> data(16, 0);

  EXPECT_FALSE(local_cursor::from_shape(shape_type_e::color, 2, 2, 8, 0, 0, std::span(data).first(15), 100));
  EXPECT_FALSE(local_cursor::from_shape(shape_type_e::color, 2, 2, 7, 0, 0, data, 100));
  EXPECT_FALSE(local_cursor::from_shape(shape_type_e::color, 0, 2, 8, 0, 0, data, 100));
  EXPECT_FALSE(local_cursor::from_shape(shape_type_e::monochrome, 8, 1, 1, 0, 0, data, 100));
  EXPECT_TRUE(local_cursor::from_shape(shape_type_e::color, 2, 2, 8, 0, 0, data, 100));
}

TEST(LocalCursor, StateChangesOnlyWithThePointer) {
  const auto before = local_cursor::state();

  local_cursor::set_visible(before.visible);
  EXPECT_EQ(local_cursor::state().serial, before.serial);

  local_cursor::set_visible(!before.visible);
  EXPECT_NE(local_cursor::state().serial, before.serial);
  local_cursor::set_visible(before.visible);

  const auto image = std::make_shared<const local_cursor::image_t>(local_cursor::image_t {1, 1, 0, 0, 1, {0, 0, 0, 0xFF}});
  const auto serial = local_cursor::state().serial;
  local_cursor::set_image(image);
  EXPECT_NE(local_cursor::state().serial, serial);
  EXPECT_EQ(local_cursor::state().image, image);
}
