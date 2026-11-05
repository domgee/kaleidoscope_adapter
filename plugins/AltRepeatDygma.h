/* -*- mode: c++ -*-
 * Kaleidoscope-AltRepeatDygma -- Previous-key alternates for Dygma keyboards
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
#include <Kaleidoscope-Ranges.h>

namespace kaleidoscope {
namespace plugin {

/* Previous-key alternates. A rule (layer, position, previous key, output) replaces the key at that position with the output
 * when the last key the host received, with its exact modifier set and side ignored, equals the previous key */
class AltRepeatDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t MAX_RULES        = 128;
  static constexpr uint8_t LAYOUT_VERSION   = 1;
  static constexpr uint8_t MAX_TRACKED_KEYS = 128;   /* largest Dygma matrix, as in CombosDygma */
  static constexpr uint8_t MODS_MASK        = CTRL_HELD | LALT_HELD | SHIFT_HELD | GUI_HELD;   /* 0x1B */
  static constexpr uint16_t REPEAT_FLAG     = 0x8000;   /* rule_t.previous bit that keeps sending the output on each press of the key */

  typedef struct PACK
  {
      uint8_t  layer;      /* owner layer, Layer.lookupActiveLayer()                  */
      uint8_t  position;   /* KeyAddr::toInt()                                        */
      uint16_t previous;   /* REPEAT_FLAG | MODS_MASK flags << 8 | keycode 0x04..0xDD */
      uint16_t output;     /* Key raw value                                           */
  } rule_t;                /* 6 B                                                     */

  typedef struct PACK
  {
      uint8_t rules_count;      /* <= MAX_RULES, else wiped at boot        */
      uint8_t layout_version;   /* != LAYOUT_VERSION => wipe (0xFF erased) */
      rule_t  rules[MAX_RULES];
  } altrepeat_config_t;         /* 770 B                                   */

  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult onFocusEvent(const char *command);
  EventHandlerResult onLayerChange();

  /* Put a replaced output back into the live keymap when a new press refreshed it while the earlier release is still queued */
  void repairCache(KeyAddr key_addr, uint8_t key_state);

  /* Return true when a rule on this layer put this key on the address at its last replayed press, and no other injected key
   * replaced it since */
  bool ruleOutputAt(KeyAddr key_addr, Key key, uint8_t layer) const;

  /* End the repeat when a key from another address types. AltRepeatTrack calls it for each key that updates history */
  void keyTyped(KeyAddr key_addr);

  static bool previousAllowed(uint16_t previous);
  static bool outputAllowed(uint16_t output);

 private:
  const altrepeat_config_t *p_config_ = nullptr;

  /* What each address holds since its last replayed press, Key_NoKey when no rule replaced it */
  Key active_out_[MAX_TRACKED_KEYS];

  /* Mark per address that active_out_ is still the output of the rule on rule_layer_ */
  uint8_t from_rule_[MAX_TRACKED_KEYS / 8];
  uint8_t rule_layer_[MAX_TRACKED_KEYS];

  /* The output a rule with REPEAT_FLAG sent at this address and owner layer, sent again on each press until another key types */
  bool    repeat_valid_  = false;
  uint8_t repeat_offset_ = 0;
  uint8_t repeat_layer_  = 0;
  Key     repeat_output_ = Key_NoKey;

  /* True while the last replayed press at repeat_offset_ sent the repeat output, so keys injected at the address keep the repeat */
  bool repeat_at_addr_ = false;

  /* True from a repeat output's press until the next event that is not injected, so the keys of a macro output keep the repeat */
  bool repeat_press_ = false;

  /* A write is parsed here in full and saved only when every value is valid */
  static altrepeat_config_t staging_;

  void setFromRule(uint8_t offset, bool value);
  bool configValid() const;
  bool canReplace(Key mapped_key, KeyAddr key_addr) const;
  const rule_t *findRule(uint8_t layer, uint8_t position) const;
  static bool replaysCell(Key mapped_key, Key cell);
  static int peekWait();
  static bool readValue(uint16_t &value);
  bool readRules();
  void sendRules() const;
  void cfgmem_config_save(const altrepeat_config_t *p_config);
};

static_assert(sizeof(AltRepeatDygma::rule_t) == 6, "AltRepeat rule_t must stay 6 B");
static_assert(sizeof(AltRepeatDygma::altrepeat_config_t) == 770, "AltRepeat config block must stay 770 B");

/* Runs the cache repair before Combos, which may buffer the new press and keep it from AltRepeat */
class AltRepeatRepairDygma : public kaleidoscope::Plugin {
 public:
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
};

/* Records the previous key the host received. Registered last, so it sees each key as it goes into the report */
class AltRepeatTrackDygma : public kaleidoscope::Plugin {
 public:
  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult afterEachCycle();

  /* The previous key for a decision. A key not yet in a report counts, with the modifiers of the report being built */
  uint16_t decisionPrevious() const;
  void commitReport(uint8_t id, const void *data, int len);

 private:
  /* Keys pressed since the last report, normally one or two. An injected release takes its key out at once.
   * 16 bounds a burst of injected presses before one report, and the oldest goes first when it is exceeded */
  static constexpr uint8_t FIFO_SIZE = 16;

  struct Pending {
    uint8_t keycode;
    uint8_t flags;   /* the key's own MODS_MASK flags, AltGr folded into Alt */
  };

  Pending  pending_[FIFO_SIZE];
  uint8_t  pending_len_ = 0;
  uint16_t previous_    = 0;   /* 0 matches no rule */
};

}
}

extern kaleidoscope::plugin::AltRepeatDygma AltRepeat;
extern kaleidoscope::plugin::AltRepeatRepairDygma AltRepeatRepair;
extern kaleidoscope::plugin::AltRepeatTrackDygma AltRepeatTrack;
