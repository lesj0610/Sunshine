/**
 * @file src/platform/keyboard_lang_keys.h
 * @brief Korean IME key (LANG1/LANG2) protocol validation.
 *
 * The streaming protocol identifies keys by Windows virtual key code, which is
 * ambiguous for the Korean and Japanese IME keys: VK_HANGUL and VK_KANA are both
 * 0x15, and VK_HANJA and VK_KANJI are both 0x19. Clients disambiguate by setting
 * SS_KBE_FLAG_LANG1 or SS_KBE_FLAG_LANG2, which carry the USB HID keyboard usage
 * the client observed (0x90 and 0x91 respectively).
 *
 * This header holds the platform-independent part of that handling so it can be
 * unit tested without a Windows toolchain.
 */
#pragma once

// standard includes
#include <cstdint>

// lib includes
#include <moonlight-common-c/src/Limelight.h>

namespace platf::keyboard {

  /**
   * @brief Virtual key code a client must pair with SS_KBE_FLAG_LANG1.
   */
  constexpr std::uint16_t lang1_key_code = 0x15;  // VK_HANGUL

  /**
   * @brief Virtual key code a client must pair with SS_KBE_FLAG_LANG2.
   */
  constexpr std::uint16_t lang2_key_code = 0x19;  // VK_HANJA

  /**
   * @brief Mask covering every LANG bit the protocol defines.
   */
  constexpr std::uint8_t lang_flag_mask = SS_KBE_FLAG_LANG1 | SS_KBE_FLAG_LANG2;

  /**
   * @brief Whether a keyboard event carries any LANG flag.
   *
   * @param flags Keyboard packet flags from the client.
   * @return True when at least one LANG bit is set.
   */
  constexpr bool has_lang_flag(std::uint8_t flags) {
    return (flags & lang_flag_mask) != 0;
  }

  /**
   * @brief Remove the LANG bits from a flags byte.
   *
   * Synthetic modifier key events reuse the flags of the key that required them.
   * The LANG bits describe one specific physical key, so they must not ride along
   * onto a synthesized Shift, Ctrl or Alt event.
   *
   * @param flags Keyboard packet flags from the client.
   * @return The same flags with every LANG bit cleared.
   */
  constexpr std::uint8_t without_lang_flags(std::uint8_t flags) {
    return static_cast<std::uint8_t>(flags & ~lang_flag_mask);
  }

  /**
   * @brief Whether a keyboard event violates the LANG flag contract.
   *
   * Only two combinations are valid: SS_KBE_FLAG_LANG1 with key code 0x15, and
   * SS_KBE_FLAG_LANG2 with key code 0x19. Both flags set at once, or either flag
   * paired with any other key code, is malformed. Such an event is dropped rather
   * than guessed at.
   *
   * @param key_code Virtual key code from the keyboard packet, masked to one byte.
   * @param flags Keyboard packet flags from the client.
   * @return True when the event must be dropped.
   */
  constexpr bool is_malformed_lang_event(std::uint16_t key_code, std::uint8_t flags) {
    const bool lang1 = (flags & SS_KBE_FLAG_LANG1) != 0;
    const bool lang2 = (flags & SS_KBE_FLAG_LANG2) != 0;

    if (lang1 && lang2) {
      return true;
    }
    if (lang1) {
      return key_code != lang1_key_code;
    }
    if (lang2) {
      return key_code != lang2_key_code;
    }
    return false;
  }

  /**
   * @brief Whether the host may schedule software key repeat for this event.
   *
   * A held Hangul or Hanja key must not auto-repeat: every repeat would toggle
   * the host IME again, so the layout would flip back and forth while the key is
   * down. Every other key keeps the existing repeat behavior.
   *
   * @param flags Keyboard packet flags from the client.
   * @return True when repeat may be scheduled.
   */
  constexpr bool should_schedule_repeat(std::uint8_t flags) {
    return !has_lang_flag(flags);
  }

}  // namespace platf::keyboard
