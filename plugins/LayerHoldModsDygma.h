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
 * Hold all modifiers for the plain shortcut with Key flag bit 5 set, 0x2000 | mods << 8 | keycode.
 * Hold only some for a partial code 0x8000 | slot << 13 | mods << 8 | keycode, whose held modifiers come from the holdmods.map table.
 * Send the other modifiers with each press. Play a partial code without a table entry as the plain shortcut */
class LayerHoldModsDygma : public kaleidoscope::Plugin {
 public:
  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult onLayerChange();
  EventHandlerResult beforeEachCycle();
  EventHandlerResult beforeReportingState();
  EventHandlerResult onFocusEvent(const char *command);

  /* The modifiers the plugin holds, in Key flags. KeyOverrides counts them as held modifiers */
  uint8_t heldMods() const {
    return held_mods_;
  }

  /* Return the layers that a held momentary layer key, or a conditional layer rule on the active and held layers, brings back on its
   * next event. DynamicMacros keeps a macro paused for such a layer */
  static uint32_t returningLayers();

  static constexpr uint8_t  HOLD_FLAG        = 0x20;
  static constexpr uint16_t RAW_MASK         = 0xE000;  /* HOLD_FLAG, SYNTHETIC and RESERVED */
  static constexpr uint16_t RAW_HOLD         = 0x2000;
  static constexpr uint8_t  MODS_MASK        = CTRL_HELD | LALT_HELD | RALT_HELD | SHIFT_HELD | GUI_HELD;
  static constexpr uint8_t  MAX_LAYERS       = 32;      /* Layer_::layer_state_ is a uint32_t */
  static constexpr uint8_t  MAX_TRACKED_KEYS = 128;     /* largest Dygma matrix, as in AutoshiftDygma */
  static constexpr uint16_t PARTIAL_MASK     = 0xC000;  /* SYNTHETIC and RESERVED */
  static constexpr uint16_t RAW_PARTIAL      = 0x8000;  /* RESERVED alone, HOLD_FLAG is the slot */
  static constexpr uint8_t  MAX_PARTIALS     = 32;
  static constexpr uint8_t  LAYOUT_VERSION   = 1;

  typedef struct PACK
  {
      uint16_t code;   /* partial code */
      uint8_t  held;   /* held modifiers, a proper non-empty subset of the code's modifiers */
  } holdmods_entry_t;  /* 3 B                                                              */

  typedef struct PACK
  {
      uint8_t          count;            /* <= MAX_PARTIALS                         */
      uint8_t          layout_version;   /* != LAYOUT_VERSION => wipe (0xFF erased) */
      holdmods_entry_t entries[MAX_PARTIALS];
  } holdmods_config_t;                   /* 98 B                                    */

  /* Accept a partial code with two or more modifiers and a typing key */
  static bool partialShape(uint16_t raw);
  static bool entryAllowed(const holdmods_entry_t &entry);
  static bool tableAllowed(const holdmods_config_t &config);

 private:
  const holdmods_config_t *p_config_ = nullptr;
  uint8_t    count_        = 0;

  /* A write is parsed here in full and saved only when the whole table is valid */
  static holdmods_config_t staging_;

  /* Track the union of held modifiers per owner layer */
  uint8_t    mods_[MAX_LAYERS] = {0};
  uint32_t   owners_       = 0;
  uint8_t    held_mods_    = 0;

  /* Mark per address a registered press, which goes out as the bare key */
  uint8_t    bare_[MAX_TRACKED_KEYS / 8] = {0};

  /* Mark per address a delivered press, so an injected duplicate does not register again */
  uint8_t    delivered_[MAX_TRACKED_KEYS / 8] = {0};

  /* Keep per address the modifiers a registered press sends with the key, so a table write does not change a press in progress */
  uint8_t    tap_[MAX_TRACKED_KEYS] = {0};

  bool       check_        = false;
  bool       report_built_ = false;
  bool       wake_pending_ = false;
  kbdtimer_t wake_timer_   = 0;

  static bool isHoldKey(Key key) {
    return (key.getRaw() & RAW_MASK) == RAW_HOLD;
  }

  static bool isPartialKey(Key key) {
    return (key.getRaw() & PARTIAL_MASK) == RAW_PARTIAL;
  }

  static bool getBit(const uint8_t *bits, KeyAddr key_addr);
  static void setBit(uint8_t *bits, KeyAddr key_addr, bool value);

  uint8_t tapAt(KeyAddr key_addr) const;
  void setTap(KeyAddr key_addr, uint8_t mods);

  uint8_t registerHold(Key key, KeyAddr key_addr);
  uint8_t partialHeld(uint16_t raw) const;
  bool ownerInactive() const;
  void dropInactiveOwners(bool in_report);
  static uint32_t heldLayerShifts();
  static void pressMods(uint8_t mods);
  static void releaseMods(uint8_t mods);
  static void pressPhysicalModifiers();

  bool configValid() const;
  bool readTable();
  void sendTable() const;
  void cfgmem_config_save(const holdmods_config_t *p_config);
};

static_assert(sizeof(LayerHoldModsDygma::holdmods_entry_t) == 3, "LayerHoldMods holdmods_entry_t must stay 3 B");
static_assert(sizeof(LayerHoldModsDygma::holdmods_config_t) == 98, "LayerHoldMods config block must stay 98 B");

}
}

extern kaleidoscope::plugin::LayerHoldModsDygma LayerHoldMods;
