/*
 * kbdfal_ll_kbdif.h - kbdfal_ll_kbdif module connects the kaleidoscope event handlers
 *                      to the keyboard API high-level interface kbdif.
 *
 * Copyright (C) 2025 Dygma Lab S.L. www.dygma.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "KbdInterface.h"
#include "Kaleidoscope.h"
#include "LEDControlDygma.h"
#include "SwapHandsDygma.h"

namespace kaleidoscope
{
namespace plugin
{
    /* Key definitions */
    typedef struct
    {
        uint16_t key;
        kbdapi_key_type_t kbdapi_key;
    } key_def_t;

    static const key_def_t p_key_def_array[] =
    {
        { .key = HID_KEYBOARD_1_AND_EXCLAMATION_POINT,  .kbdapi_key = KBDAPI_KEY_TYPE_KBD_1_AND_EXCLAMATION_POINT },
        { .key = HID_KEYBOARD_2_AND_AT,                 .kbdapi_key = KBDAPI_KEY_TYPE_KBD_2_AND_AT },
        { .key = HID_KEYBOARD_3_AND_POUND,              .kbdapi_key = KBDAPI_KEY_TYPE_KBD_3_AND_POUND },
        { .key = HID_KEYBOARD_4_AND_DOLLAR,             .kbdapi_key = KBDAPI_KEY_TYPE_KBD_4_AND_DOLLAR },
        { .key = HID_KEYBOARD_5_AND_PERCENT,            .kbdapi_key = KBDAPI_KEY_TYPE_KBD_5_AND_PERCENT },
        { .key = HID_KEYBOARD_6_AND_CARAT,              .kbdapi_key = KBDAPI_KEY_TYPE_KBD_6_AND_CARAT },
        { .key = HID_KEYBOARD_7_AND_AMPERSAND,          .kbdapi_key = KBDAPI_KEY_TYPE_KBD_7_AND_AMPERSAND },
        { .key = HID_KEYBOARD_8_AND_ASTERISK,           .kbdapi_key = KBDAPI_KEY_TYPE_KBD_8_AND_ASTERISK },
        { .key = HID_KEYBOARD_9_AND_LEFT_PAREN,         .kbdapi_key = KBDAPI_KEY_TYPE_KBD_9_AND_LEFT_PAREN },
        { .key = HID_KEYBOARD_0_AND_RIGHT_PAREN,        .kbdapi_key = KBDAPI_KEY_TYPE_KBD_0_AND_RIGHT_PAREN },

        { .key = ranges::BATTERY_LEVEL,                 .kbdapi_key = KBDAPI_KEY_TYPE_BATTERY_LEVEL },
        { .key = ranges::BLUETOOTH_PAIRING,             .kbdapi_key = KBDAPI_KEY_TYPE_BLUETOOTH_PAIRING },
        { .key = ranges::OVERLAY_KEY,                   .kbdapi_key = KBDAPI_KEY_TYPE_OVERLAY },
        { .key = ranges::OVERLAY_TAP,                   .kbdapi_key = KBDAPI_KEY_TYPE_OVERLAY_TAP },
        { .key = ranges::OVERLAY_HOLD,                  .kbdapi_key = KBDAPI_KEY_TYPE_OVERLAY_HOLD },
    };
    #define get_key_def( def, id ) _get_def( def, p_key_def_array, key_def_t, key, id )

    static inline EventHandlerResult EventHandlerResult_get( kbdapi_event_result_t event_result )
    {
        switch( event_result )
        {
            case KBDAPI_EVENT_RESULT_CONSUMED:
                return EventHandlerResult::EVENT_CONSUMED;

            case KBDAPI_EVENT_RESULT_ERROR:
                return EventHandlerResult::ERROR;

            case KBDAPI_EVENT_RESULT_IGNORED:
            default:
                return EventHandlerResult::OK;
        }
    }

    EventHandlerResult KbdInterface::onKeyswitchEvent(Key &mappedKey, KeyAddr key_addr, uint8_t keyState)
    {
        kbdapi_key_t kbdapi_key;
        kbdapi_event_result_t event_result;

        const key_def_t * p_key_def;

        /* Get the key definition */
        get_key_def( p_key_def, mappedKey.getRaw() );

        /* Populate Keyboard API key structure */
        kbdapi_key.type = ( p_key_def != NULL ) ? p_key_def->kbdapi_key : KBDAPI_KEY_TYPE_UNSPECIFIED;

        /* Fill the physical key coordinates, which differ from the address while swap hands mirrors the key */
        KeyAddr phys_addr = SwapHands.physicalFor( key_addr );

        kbdapi_key.coord.is_valid = phys_addr.isValid();
        kbdapi_key.coord.col = phys_addr.col();
        kbdapi_key.coord.row = phys_addr.row();

        kbdapi_key.is_pressed = keyIsPressed(keyState);
        kbdapi_key.was_pressed = keyWasPressed(keyState);
        kbdapi_key.toggled_on = keyToggledOn(keyState);
        kbdapi_key.toggled_off = keyToggledOff(keyState);
        kbdapi_key.injected = ( keyState & INJECTED ) ? true : false;

        event_result = kbdif_key_event( p_kbdif, &kbdapi_key );

        return EventHandlerResult_get( event_result );
    }

    EventHandlerResult KbdInterface::onFocusEvent(const char * p_command)
    {
        kbdapi_event_result_t event_result;

        event_result = kbdif_command_event( p_kbdif, p_command );

        return EventHandlerResult_get( event_result );
    }

    EventHandlerResult KbdInterface::onLayerChange()
    {
        kbdapi_event_result_t event_result = KBDAPI_EVENT_RESULT_IGNORED;
        kbdapi_led_layer_id_t layer_id = Layer.mostRecent();

        event_result = kbdif_led_layer_change_event( p_kbdif, layer_id );

        return EventHandlerResult_get( event_result );
    }

    EventHandlerResult KbdInterface::onLEDModeChange()
    {
        kbdapi_event_result_t event_result = KBDAPI_EVENT_RESULT_IGNORED;
        kbdapi_led_effect_action_t led_effect_action = ::LEDControl.getCurrentAction();

        event_result = kbdif_led_effect_change_event( p_kbdif, led_effect_action );

        return EventHandlerResult_get( event_result );
    }

    result_t KbdInterface::kbdifRegister( kbdif_t * p_kbdif )
    {
        this->p_kbdif = p_kbdif;

        return RESULT_OK;
    }

} // namespace plugin
} //  namespace kaleidoscope

kaleidoscope::plugin::KbdInterface KbdInterface;
