/* -*- mode: c++ -*-
 * Kaleidoscope-KeyOverridesDygma -- QMK-style key overrides for Dygma keyboards
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

/* QMK-style key overrides. A held trigger key with the required modifiers, no forbidden modifier and an allowed layer sends the
 * replacement instead, and the suppressed modifiers leave every report while the override is active */
class KeyOverridesDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t  MAX_OVERRIDES    = 32;
  static constexpr uint8_t  LAYOUT_VERSION   = 1;
  static constexpr uint8_t  MAX_LAYERS       = 10;
  static constexpr uint16_t LAYERS_MASK      = 0x03FF;
  static constexpr uint8_t  MAX_TRACKED_KEYS = 128;   /* largest Dygma matrix, as in CombosDygma */
  static constexpr uint8_t  MODS_MASK        = CTRL_HELD | LALT_HELD | SHIFT_HELD | GUI_HELD;   /* 0x1B, side ignored */
  static constexpr uint8_t  REPLACEMENT_MODS = MODS_MASK | RALT_HELD;                            /* 0x1F, AltGr allowed */

  /* QMK ko_option_t bits */
  static constexpr uint8_t OPT_TRIGGER_DOWN        = 0x01;
  static constexpr uint8_t OPT_REQUIRED_MOD_DOWN   = 0x02;
  static constexpr uint8_t OPT_NEGATIVE_MOD_UP     = 0x04;
  static constexpr uint8_t OPT_ALL_ACTIVATIONS     = 0x07;
  static constexpr uint8_t OPT_ONE_MOD             = 0x08;
  static constexpr uint8_t OPT_NO_REREGISTER       = 0x10;
  static constexpr uint8_t OPT_NO_UNREGISTER_OTHER = 0x20;
  static constexpr uint8_t OPTIONS_MASK            = 0x3F;

  static constexpr uint16_t REPEAT_DELAY_MS = 500;   /* QMK KEY_OVERRIDE_REPEAT_DELAY */
  static constexpr uint16_t SHORT_DELAY_MS  = 50;

  typedef struct PACK
  {
      uint8_t  trigger;           /* HID keycode 0x04..0xDD                                 */
      uint8_t  trigger_mods;      /* MODS_MASK                                              */
      uint16_t layers;            /* bit n = layer n, not 0, subset of LAYERS_MASK          */
      uint8_t  negative_mods;     /* MODS_MASK, no bit of trigger_mods                      */
      uint8_t  suppressed_mods;   /* MODS_MASK                                              */
      uint16_t replacement;       /* 0 = NoKey, else REPLACEMENT_MODS << 8 | keycode 4..DD  */
      uint8_t  options;           /* OPTIONS_MASK                                           */
      uint8_t  enabled;           /* 0 or 1                                                 */
  } override_t;                   /* 10 B                                                   */

  typedef struct PACK
  {
      uint8_t    count;            /* <= MAX_OVERRIDES, list order is priority */
      uint8_t    layout_version;   /* != LAYOUT_VERSION => wipe (0xFF erased)  */
      override_t overrides[MAX_OVERRIDES];
  } keyoverrides_config_t;         /* 322 B                                    */

  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult beforeEachCycle();
  EventHandlerResult beforeReportingState();
  EventHandlerResult onFocusEvent(const char *command);

  static bool overrideAllowed(const override_t &o);

 private:
  enum class Deferred : uint8_t { NONE, REPLACEMENT, TRIGGER };
  static constexpr uint8_t NO_ADDR = 0xFF;

  const keyoverrides_config_t *p_config_ = nullptr;
  uint8_t count_ = 0;

  /* The one active override, QMK active_override. Keep copies so a config write never changes what deactivation undoes */
  bool     active_valid_       = false;
  uint8_t  active_index_       = 0;
  uint8_t  active_addr_        = NO_ADDR;
  bool     active_registered_  = false;   /* false while a deferred replacement waits */
  uint16_t active_replacement_ = 0;
  uint8_t  active_options_     = 0;
  bool     replacement_fresh_  = false;   /* the replacement toggled on in this cycle */

  /* The last non-modifier key pressed and still held, QMK last_key_down */
  uint8_t  last_addr_    = NO_ADDR;
  uint8_t  last_keycode_ = 0;   /* 0 when the key can not be a trigger */
  uint8_t  last_layer_   = 0;
  uint32_t last_down_ms_ = 0;

  /* One deferred register, QMK deferred_register */
  Deferred   deferred_      = Deferred::NONE;
  uint8_t    deferred_addr_ = NO_ADDR;
  kbdtimer_t timer_         = 0;

  /* Wake once more when an active trigger lost its key without a release, so the clean report goes out */
  kbdtimer_t wake_         = 0;
  bool       wake_pending_ = false;

  /* The next held event here after a deferred register goes into AltRepeat history */
  uint8_t note_addr_ = NO_ADDR;

  /* Modifier bits by HID side that each address holds, refreshed by its modifier events every cycle, and a count per bit */
  uint8_t mod_hid_[MAX_TRACKED_KEYS]      = {0};
  uint8_t mod_addr_[MAX_TRACKED_KEYS / 8] = {0};
  uint8_t mod_seen_[MAX_TRACKED_KEYS / 8] = {0};
  bool    mod_seen_any_                   = false;
  uint8_t hid_count_[8]                   = {0};
  uint8_t held_hid_                       = 0;
  uint8_t mods_last_                      = 0;   /* folded mods the plugin last processed */

  /* Keep an address out of the report until it is released, and tell an injected duplicate press from a new one */
  uint8_t muted_[MAX_TRACKED_KEYS / 8]      = {0};
  uint8_t down_[MAX_TRACKED_KEYS / 8]       = {0};
  uint8_t down_stale_[MAX_TRACKED_KEYS / 8] = {0};
  bool    muted_any_ = false;
  bool    down_any_  = false;

  /* A write is parsed here in full and saved only when every value is valid */
  static keyoverrides_config_t staging_;

  bool configValid() const;
  bool isHeld(uint8_t offset) const;
  void setModContribution(uint8_t offset, uint8_t hid);
  uint8_t transitionMods() const;
  static uint8_t oneShotMods();
  static bool modsMatch(const override_t &o, uint8_t mods);
  static bool activationAllowed(uint8_t options, bool is_mod, bool key_down);
  void pressAt(Key key, KeyAddr key_addr);
  void releaseAt(uint8_t offset);
  EventHandlerResult outputAt(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  bool tryActivate(bool is_mod, bool key_down, uint8_t mods);
  void activate(uint8_t index, bool via_modifier);
  void deactivate(bool allow_reregister);
  void sendFreshReplacement();
  bool modEvent(bool key_down);
  void maskChanged();
  void scheduleDeferred(Deferred kind, uint8_t offset);
  void cancelDeferred();
  void fireDeferred();
  void setMuted(uint8_t offset, bool value);
  void sweep();
  void endAll(bool clear_mods);
  bool readOverrides();
  void sendOverrides() const;
  void cfgmem_config_save(const keyoverrides_config_t *p_config);
};

static_assert(sizeof(KeyOverridesDygma::override_t) == 10, "KeyOverrides override_t must stay 10 B");
static_assert(sizeof(KeyOverridesDygma::keyoverrides_config_t) == 322, "KeyOverrides config block must stay 322 B");

}
}

extern kaleidoscope::plugin::KeyOverridesDygma KeyOverrides;
