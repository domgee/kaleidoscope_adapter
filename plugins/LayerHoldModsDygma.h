/* -*- mode: c++ -*-
 * Kaleidoscope-LayerHoldModsDygma -- Hold shortcut modifiers until their layer turns off
 * Copyright (C) 2026 Dygma Lab S.L. www.dygma.com
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

namespace kaleidoscope {
namespace plugin {

/* Keep a shortcut's modifiers held until the layer the key sits on turns off.
 * Keymap encoding is the plain shortcut with Key flag bit 5 set, 0x2000 | mods << 8 | keycode */
class LayerHoldModsDygma : public kaleidoscope::Plugin {
 public:
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult onLayerChange();
  EventHandlerResult beforeEachCycle();
  EventHandlerResult beforeReportingState();
  EventHandlerResult onFocusEvent(const char *command);

  /* The modifiers the plugin holds, in Key flags. KeyOverrides counts them as held modifiers */
  uint8_t heldMods() const {
    return held_mods_;
  }

  static constexpr uint8_t  HOLD_FLAG        = 0x20;
  static constexpr uint16_t RAW_MASK         = 0xE000;  /* HOLD_FLAG, SYNTHETIC and RESERVED */
  static constexpr uint16_t RAW_HOLD         = 0x2000;
  static constexpr uint8_t  MODS_MASK        = CTRL_HELD | LALT_HELD | RALT_HELD | SHIFT_HELD | GUI_HELD;
  static constexpr uint8_t  MAX_LAYERS       = 32;      /* Layer_::layer_state_ is a uint32_t */
  static constexpr uint8_t  MAX_TRACKED_KEYS = 128;     /* largest Dygma matrix, as in AutoshiftDygma */

 private:
  /* Track the union of held modifiers per owner layer */
  uint8_t    mods_[MAX_LAYERS] = {0};
  uint32_t   owners_       = 0;
  uint8_t    held_mods_    = 0;

  /* Mark per address a registered press, which goes out as the bare key */
  uint8_t    bare_[MAX_TRACKED_KEYS / 8] = {0};

  /* Mark per address a delivered press, so an injected duplicate does not register again */
  uint8_t    delivered_[MAX_TRACKED_KEYS / 8] = {0};

  bool       check_        = false;
  bool       report_built_ = false;
  bool       wake_pending_ = false;
  kbdtimer_t wake_timer_   = 0;

  static bool isHoldKey(Key key) {
    return (key.getRaw() & RAW_MASK) == RAW_HOLD;
  }

  static bool getBit(const uint8_t *bits, KeyAddr key_addr);
  static void setBit(uint8_t *bits, KeyAddr key_addr, bool value);

  bool registerHold(Key key, KeyAddr key_addr);
  bool ownerInactive() const;
  void dropInactiveOwners(bool in_report);
  static uint32_t heldLayerShifts();
  static void pressMods(uint8_t mods);
  static void releaseMods(uint8_t mods);
  static void pressPhysicalModifiers();
};

}
}

extern kaleidoscope::plugin::LayerHoldModsDygma LayerHoldMods;
