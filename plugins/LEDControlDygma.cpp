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

#include "kaleidoscope/keyswitch_state.h"
#include "LEDControlDygma.h"
#include "LEDCustomDygma.h"

using namespace kaleidoscope::internal; // NOLINT(build/namespaces)

namespace kaleidoscope
{
namespace plugin
{

Key LEDControl::pending_next_prev_key_ = Key_NoKey;
kbdapi_led_effect_action_t LEDControl::led_effect_action = KBDAPI_LED_EFFECT_ACTION_NONE;
LEDControl::custom_handler_t LEDControl::custom_handler_ = nullptr;
uint8_t LEDControl::custom_down_[LEDControl::CUSTOM_TRACKED_KEYS / 8] = {0};

LEDControl::LEDControl(void)
{
}

void LEDControl::hook_trigger( kbdapi_led_effect_action_t action )
{
    /* Prepare the led effect action to be possibly obtained from within the hooks */
    led_effect_action = action;

    /* Process the hooks */
    kaleidoscope::Hooks::onLEDModeChange();

    /* Reset the led effect action */
    led_effect_action = KBDAPI_LED_EFFECT_ACTION_NONE;
}

INLINE void LEDControl::next_mode(void)
{
    hook_trigger( KBDAPI_LED_EFFECT_ACTION_NEXT );
}

INLINE void LEDControl::prev_mode(void)
{
    hook_trigger( KBDAPI_LED_EFFECT_ACTION_PREVIOUS );
}

INLINE void LEDControl::leds_toggle( void )
{
    hook_trigger( KBDAPI_LED_EFFECT_ACTION_TOGGLE );
}

kbdapi_led_effect_action_t LEDControl::getCurrentAction(void)
{
    return led_effect_action;
}

void LEDControl::setCustomHandler(custom_handler_t handler)
{
    custom_handler_ = handler;
}

void LEDControl::custom_toggle(uint8_t slot)
{
    led_custom_t settings;

    if (custom_handler_ == nullptr)
    {
        return;
    }

    /* Do nothing for an unused slot */
    if (!LEDCustom.slotGet(slot, settings))
    {
        return;
    }

    custom_handler_(slot, settings);
}

/* Skip a Superkeys timeline delivery that repeats a slot press the address already made, as KeyOverrides does. The timeline injects it
 * with INJECTED on the next cycle, also after a Qukeys replay or a combo. Address-less presses such as macros are never skipped */
bool LEDControl::custom_press_duplicate(KeyAddr key_addr, uint8_t keyState)
{
    if (!key_addr.isValid() || key_addr.toInt() >= CUSTOM_TRACKED_KEYS)
    {
        return false;
    }

    uint8_t offset = key_addr.toInt();
    uint8_t mask = (uint8_t)(1 << (offset % 8));

    if ((keyState & INJECTED) && (custom_down_[offset / 8] & mask))
    {
        return true;
    }

    custom_down_[offset / 8] |= mask;

    return false;
}

void LEDControl::custom_release(KeyAddr key_addr)
{
    if (!key_addr.isValid() || key_addr.toInt() >= CUSTOM_TRACKED_KEYS)
    {
        return;
    }

    uint8_t offset = key_addr.toInt();

    custom_down_[offset / 8] &= (uint8_t)~(1 << (offset % 8));
}

kaleidoscope::EventHandlerResult LEDControl::onSetup()
{
    return EventHandlerResult::OK;
}

kaleidoscope::EventHandlerResult LEDControl::onKeyswitchEvent(Key &mappedKey, KeyAddr key_addr, uint8_t keyState)
{
    /* Clear the slot press at any release of the address, also when the live key there is no longer a slot key */
    if (keyToggledOff(keyState))
    {
        custom_release(key_addr);
    }

    if (mappedKey.getFlags() != (SYNTHETIC | IS_INTERNAL | LED_TOGGLE)) return kaleidoscope::EventHandlerResult::OK;

    if (keyToggledOn(keyState))
    {
        if (mappedKey == Key_LEDEffectNext || mappedKey == Key_LEDEffectPrevious)
        {
            // Handling of these keys is delayed into `beforeReportingState`
            // so that we can incorporate the shift modifier state.
            pending_next_prev_key_ = mappedKey;
        }
        else if (mappedKey == Key_LEDToggle)
        {
            leds_toggle();
        }
        else if (mappedKey.getKeyCode() >= LED_CUSTOM_KEY_FIRST && mappedKey.getKeyCode() < LED_CUSTOM_KEY_FIRST + LED_CUSTOM_SLOT_COUNT)
        {
            /* Act on the press only, so a duplicate release can not toggle twice */
            if (!custom_press_duplicate(key_addr, keyState))
            {
                custom_toggle(mappedKey.getKeyCode() - LED_CUSTOM_KEY_FIRST);
            }
        }
    }

    return kaleidoscope::EventHandlerResult::EVENT_CONSUMED;
}

kaleidoscope::EventHandlerResult LEDControl::beforeReportingState(void)
{
    if (pending_next_prev_key_ != Key_NoKey)
    {
        bool is_shifted =
            kaleidoscope::Runtime.hid().keyboard().isModifierKeyActive(Key_LeftShift) || kaleidoscope::Runtime.hid().keyboard().isModifierKeyActive(Key_RightShift);

        if ((pending_next_prev_key_ == Key_LEDEffectNext && !is_shifted) || (pending_next_prev_key_ == Key_LEDEffectPrevious && is_shifted))
        {
            next_mode();
        }
        else
        {
            prev_mode();
        }
        pending_next_prev_key_ = Key_NoKey;
    }

    return kaleidoscope::EventHandlerResult::OK;
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::LEDControl LEDControl;
