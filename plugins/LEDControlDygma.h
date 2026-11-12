/* LEDControlDygma - LED control plugin for Kaleidoscope
 * Copyright (C) 2017-2020  Keyboard.io, Inc.
 * Copyright (C) 2025 Dygma Lab S.L. www.dygma.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "kbd_core.h"

#include "kaleidoscope/Runtime.h"
#include "kaleidoscope/plugin/LEDMode.h"
#include "LEDEffect-Custom-Defy.h"

#define LED_TOGGLE 0b00000001 // Synthetic, internal

#define Key_LEDEffectNext Key(0, KEY_FLAGS | SYNTHETIC | IS_INTERNAL | LED_TOGGLE)
#define Key_LEDEffectPrevious Key(1, KEY_FLAGS | SYNTHETIC | IS_INTERNAL | LED_TOGGLE)
#define Key_LEDToggle Key(2, KEY_FLAGS | SYNTHETIC | IS_INTERNAL | LED_TOGGLE)

/* Custom effect slots 0..LED_CUSTOM_SLOT_COUNT - 1 are 0x4303..0x4312 */
#define LED_CUSTOM_KEY_FIRST 3
#define Key_LEDCustom(n) Key(LED_CUSTOM_KEY_FIRST + (n), KEY_FLAGS | SYNTHETIC | IS_INTERNAL | LED_TOGGLE)

namespace kaleidoscope
{
namespace plugin
{

class LEDMode;

class LEDControl : public kaleidoscope::Plugin
{
  public:
    LEDControl(void);

    kaleidoscope::EventHandlerResult onSetup();
    kaleidoscope::EventHandlerResult onKeyswitchEvent(Key &mappedKey, KeyAddr key_addr, uint8_t keyState);
    kaleidoscope::EventHandlerResult beforeReportingState();

    static kbdapi_led_effect_action_t getCurrentAction(void);

    typedef void (*custom_handler_t)(uint8_t slot, const led_custom_t &settings);

    /* Set the function that turns a custom effect slot on or off */
    static void setCustomHandler(custom_handler_t handler);

  private:
    static Key pending_next_prev_key_;
    static kbdapi_led_effect_action_t led_effect_action;
    static custom_handler_t custom_handler_;

    /* Addresses whose slot key press LEDControl took and whose release has not come yet */
    static constexpr uint8_t CUSTOM_TRACKED_KEYS = 128;   /* largest Dygma matrix, as in CombosDygma */
    static uint8_t custom_down_[CUSTOM_TRACKED_KEYS / 8];

    static INLINE void next_mode(void);
    static INLINE void prev_mode(void);
    static INLINE void leds_toggle(void);
    static void hook_trigger( kbdapi_led_effect_action_t action );
    static void custom_toggle(uint8_t slot);
    static bool custom_press_duplicate(KeyAddr key_addr, uint8_t keyState);
    static void custom_release(KeyAddr key_addr);
};

} // namespace plugin
} // namespace kaleidoscope

extern kaleidoscope::plugin::LEDControl LEDControl;
