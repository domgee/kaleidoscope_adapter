/*
 * kbdfal_ll_base.cpp - This file contains sources for configuring and enabling
 *                      kaleidoscope_adapter base.
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

#include "AltRepeatDygma.h"
#include "AutoshiftDygma.h"
#include "BaseDygma.h"
#include "CapsWordDygma.h"
#include "CombosDygma.h"
#include "ConditionalLayersDygma.h"
#include "DynamicMacrosDygma.h"
#include "EEPROMKeymapDygma.h"
#include "kbdfal_ll_base.h"
#include "KbdInterface.h"
#include "KeyOverridesDygma.h"
#include "LayerHoldModsDygma.h"
#include "LEDControlDygma.h"
#include "LEDCustomDygma.h"
#include "MouseKeysDygma.h"
#include "OneShotDygma.h"
#include "SwapHandsDygma.h"

// Kaleidoscope
#include "Kaleidoscope-FocusSerial.h"
#include "Kaleidoscope-KeyRoleManager.h"
#include "Kaleidoscope-LayerFocus.h"
#include "Kaleidoscope.h"

#include "kaleidoscope/device/dygma/keyboardManager/universalModules/Focus.h"
#include "kaleidoscope/Runtime.h"

// Support for host power management (suspend & wakeup)
#include "Kaleidoscope-HostPowerManagement.h"

#ifndef KALEIDOSCOPE_ADAPTER_KEYMAP_DEFAULT
#error "Default Kaleidoscope Adapter Keymap is not specified"
#endif /* KALEIDOSCOPE_ADAPTER_KEYMAP_DEFAULT */

KEYMAPS
(
    KALEIDOSCOPE_ADAPTER_KEYMAP_DEFAULT
);

KALEIDOSCOPE_INIT_PLUGINS
(
    EEPROMKeymap,
    /*ConditionalLayers follows EEPROMKeymap and precedes every other onLayerChange consumer, so the outer layer change reaches
      them after the rules applied*/
    ConditionalLayers,
    /*SwapHands precedes every key plugin. Its scanner router mirrors addresses before the core sees an event, and its hook turns SH_T
      codes into their tap key and consumes the other swap codes*/
    SwapHands,
    /*AltRepeatRepair precedes Combos, which may buffer a re-press before AltRepeat and RepeatKey see it*/
    AltRepeatRepair,
    /*CapsWord trigger and Autoshift — must precede keyRoleManager so their own
      ranges are consumed before the Qukeys queue and the SuperKeys timeline see
      them. That keeps them out of both delay stages and of the timeline's
      double delivery.*/
    /*Combos goes first of all: it matches on physical position, so it must see
      raw keyswitch events before any plugin can consume them -- including
      Autoshift, which owns its own range.*/
    Combos,
    /*AltRepeat follows Combos and precedes keyRoleManager. It replaces a key on its Qukeys replay, before CapsWordTrigger,
      Autoshift and Superkeys see it*/
    AltRepeat,
    CapsWordTrigger, Autoshift,
    keyRoleManager,
    /*RepeatKey follows keyRoleManager to resolve Qukeys replays, Superkey actions and timeline deliveries in logical order, and precedes
      LayerHoldMods, KeyOverrides, OneShot, CapsWordShift and AltRepeatTrack so they see the repeated key*/
    RepeatKey,
    /*Run after keyRoleManager to see resolved Qukeys replays and Superkeys
      deliveries, and strip the hold flag before KbdInterface matches exact raw values*/
    LayerHoldMods,
    /*KeyOverrides follows LayerHoldMods to see resolved Qukeys replays, Superkeys deliveries and bare layer hold keys, and
      precedes OneShot, LEDControl, DynamicMacros and MouseKeys so it sees the keys they consume as other keys*/
    KeyOverrides,
    LEDControl,
    /*LEDCustom only loads the custom LED effect table and answers led.custom. LEDControl looks slots up in it*/
    LEDCustom,
    KeyboardFocus, DynamicMacros,
    /*SwapHandsOneShot judges SH_OS on the same resolved keys OneShot sees*/
    /*SideFlash,*/ Focus, MouseKeys, SwapHandsOneShot, OneShot, LayerFocus,
    HostPowerManagement,
    /*Dygma base plugin*/
    BaseDygma,
    /*Keyboard API interface*/
    KbdInterface,
    /*CapsWord shift — must follow KbdInterface, which looks its key table up by
      the exact raw value and would miss a key carrying SHIFT_HELD*/
    CapsWordShift,
    /*AltRepeatTrack runs last to see each key as it goes into the report*/
    AltRepeatTrack
);

result_t kbdfal_ll_base_init( void )
{
    Kaleidoscope.setup();

    return RESULT_OK;
}

result_t kbdfal_ll_base_key_report_enable( void )
{
    return BaseDygma.key_report_enable();
}

result_t kbdfal_ll_base_key_report_disable( void )
{
    return BaseDygma.key_report_disable();
}

result_t kbdfal_ll_base_kbdif_set( kbdif_t * p_kbdif )
{
    result_t result = RESULT_ERR;

    result = KbdInterface.kbdifRegister( p_kbdif );
    EXIT_IF_ERR( result, "KbdInterface.kbdifRegister failed" );

_EXIT:
    return result;
}
