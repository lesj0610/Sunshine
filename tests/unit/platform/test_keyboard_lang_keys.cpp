/**
 * @file tests/unit/platform/test_keyboard_lang_keys.cpp
 * @brief Tests for Korean IME key (LANG1/LANG2) validation and scan code mapping.
 */
// local includes
#include "src/platform/keyboard_lang_keys.h"

// test includes
#include <gtest/gtest.h>

namespace {

  constexpr std::uint8_t lang1 = SS_KBE_FLAG_NON_NORMALIZED | SS_KBE_FLAG_LANG1;
  constexpr std::uint8_t lang2 = SS_KBE_FLAG_NON_NORMALIZED | SS_KBE_FLAG_LANG2;

  TEST(KeyboardLangKeys, ValidLang1MapsToHangulScanCode) {
    const auto scan_code = platf::keyboard::lang_scan_code(0x15, lang1);
    ASSERT_TRUE(scan_code.has_value());
    EXPECT_EQ(*scan_code, 0xF2);
    EXPECT_FALSE(platf::keyboard::is_malformed_lang_event(0x15, lang1));
  }

  TEST(KeyboardLangKeys, ValidLang2MapsToHanjaScanCode) {
    const auto scan_code = platf::keyboard::lang_scan_code(0x19, lang2);
    ASSERT_TRUE(scan_code.has_value());
    EXPECT_EQ(*scan_code, 0xF1);
    EXPECT_FALSE(platf::keyboard::is_malformed_lang_event(0x19, lang2));
  }

  TEST(KeyboardLangKeys, LangFlagWithWrongKeyCodeIsMalformed) {
    // LANG1 belongs to VK_HANGUL only, LANG2 to VK_HANJA only.
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x19, lang1));
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x15, lang2));
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x41, lang1));
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x41, lang2));

    EXPECT_FALSE(platf::keyboard::lang_scan_code(0x19, lang1).has_value());
    EXPECT_FALSE(platf::keyboard::lang_scan_code(0x15, lang2).has_value());
  }

  TEST(KeyboardLangKeys, BothLangFlagsIsMalformed) {
    constexpr std::uint8_t both = SS_KBE_FLAG_NON_NORMALIZED | SS_KBE_FLAG_LANG1 | SS_KBE_FLAG_LANG2;

    // Malformed for every key code, including the two otherwise valid ones.
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x15, both));
    EXPECT_TRUE(platf::keyboard::is_malformed_lang_event(0x19, both));
    EXPECT_FALSE(platf::keyboard::lang_scan_code(0x15, both).has_value());
    EXPECT_FALSE(platf::keyboard::lang_scan_code(0x19, both).has_value());
  }

  TEST(KeyboardLangKeys, OrdinaryKeysAreUnaffected) {
    // No LANG bits: never malformed, never a scan code override, flags untouched.
    for (const std::uint8_t flags : {std::uint8_t {0}, std::uint8_t {SS_KBE_FLAG_NON_NORMALIZED}}) {
      for (const std::uint16_t key_code : {0x41, 0x15, 0x19, 0x1C, 0x1D, 0xA2}) {
        EXPECT_FALSE(platf::keyboard::is_malformed_lang_event(key_code, flags));
        EXPECT_FALSE(platf::keyboard::lang_scan_code(key_code, flags).has_value());
      }
      EXPECT_FALSE(platf::keyboard::has_lang_flag(flags));
      EXPECT_EQ(platf::keyboard::without_lang_flags(flags), flags);
    }
  }

  TEST(KeyboardLangKeys, WithoutLangFlagsClearsOnlyLangBits) {
    EXPECT_EQ(platf::keyboard::without_lang_flags(lang1), SS_KBE_FLAG_NON_NORMALIZED);
    EXPECT_EQ(platf::keyboard::without_lang_flags(lang2), SS_KBE_FLAG_NON_NORMALIZED);

    // Unknown bits are forwarded unchanged; only the LANG bits are cleared.
    constexpr std::uint8_t unknown = 0x40;
    EXPECT_EQ(platf::keyboard::without_lang_flags(lang1 | unknown), SS_KBE_FLAG_NON_NORMALIZED | unknown);
  }

  TEST(KeyboardLangKeys, HasLangFlagDetectsLangEvents) {
    EXPECT_TRUE(platf::keyboard::has_lang_flag(lang1));
    EXPECT_TRUE(platf::keyboard::has_lang_flag(lang2));
    EXPECT_FALSE(platf::keyboard::has_lang_flag(SS_KBE_FLAG_NON_NORMALIZED));
    EXPECT_FALSE(platf::keyboard::has_lang_flag(0));
  }

  TEST(KeyboardLangKeys, RepeatIsSuppressedOnlyForLangKeys) {
    EXPECT_FALSE(platf::keyboard::should_schedule_repeat(lang1));
    EXPECT_FALSE(platf::keyboard::should_schedule_repeat(lang2));
    EXPECT_TRUE(platf::keyboard::should_schedule_repeat(0));
    EXPECT_TRUE(platf::keyboard::should_schedule_repeat(SS_KBE_FLAG_NON_NORMALIZED));
  }

}  // namespace
