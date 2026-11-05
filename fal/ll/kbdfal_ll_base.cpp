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
#include "DynamicMacrosDygma.h"
#include "EEPROMKeymapDygma.h"
#include "kbdfal_ll_base.h"
#include "KbdInterface.h"
#include "LayerHoldModsDygma.h"
#include "LEDControlDygma.h"
#include "MouseKeysDygma.h"
#include "OneShotDygma.h"

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
    /*AltRepeatRepair precedes Combos, which may buffer a re-press before AltRepeat sees it*/
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
    /*Run after keyRoleManager to see resolved Qukeys replays and Superkeys
      deliveries, and strip the hold flag before KbdInterface matches exact raw values*/
    LayerHoldMods,
    LEDControl, KeyboardFocus, DynamicMacros,
    /*SideFlash,*/ Focus, MouseKeys, OneShot, LayerFocus,
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
