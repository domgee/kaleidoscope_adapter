/* -*- mode: c++ -*-
 * Kaleidoscope-DominantLayerDygma -- play a key on its dominant layer when that layer turns on right after the press
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

/* Hold a key with a dominant layer back when it is pressed on layer 0. Replay it once on its dominant layer when that layer turns on
 * before the timeout and before any other event, else on layer 0. Qukeys times the replay from the physical press */
class DominantLayerDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t  LAYOUT_VERSION     = 1;
  static constexpr uint8_t  POSITIONS          = 60;
  static constexpr uint8_t  NO_LAYER           = 0xFF;
  static constexpr uint8_t  MIN_LAYER          = 1;
  static constexpr uint8_t  MAX_LAYER          = 9;
  static constexpr uint16_t TIMEOUT_MIN_MS     = 10;
  static constexpr uint16_t TIMEOUT_MAX_MS     = 250;
  static constexpr uint16_t TIMEOUT_DEFAULT_MS = 60;

  typedef struct PACK
  {
      uint8_t  layout_version;    /* != LAYOUT_VERSION => defaults (0xFF erased) */
      uint8_t  reserved;
      uint16_t timeout_ms;        /* TIMEOUT_MIN_MS..TIMEOUT_MAX_MS              */
      uint8_t  layer[POSITIONS];  /* MIN_LAYER..MAX_LAYER, NO_LAYER = none       */
  } dominant_config_t;            /* 64 B                                        */

  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult beforeReportingState();
  EventHandlerResult onLayerChange();
  EventHandlerResult onFocusEvent(const char *command);

  /* Replay a waiting key on layer 0 now. EEPROMKeymap calls it before a keymap write */
  void settle();

  /* Return true while the held-back press is replayed. DynamicMacros saw its physical press at the hold-back already */
  bool isReplayingHeldPress() const { return replaying_; }

 private:
  /* Positions 52..55 have no key */
  static constexpr uint8_t GAP_FIRST   = 52;
  static constexpr uint8_t GAP_LAST    = 55;
  static constexpr uint8_t MAX_ENTRIES = POSITIONS - (GAP_LAST - GAP_FIRST + 1);

  enum class State : uint8_t { IDLE, WAITING };
  enum class Kind : uint8_t { DEFAULT, DOMINANT };

  const dominant_config_t *p_config_ = nullptr;

  State    state_ = State::IDLE;
  KeyAddr  addr_;
  Key      k0_ = Key_NoKey;
  uint8_t  layer_ = NO_LAYER;
  uint32_t press_ms_ = 0;

  /* Deadline of the wait, and a 1 ms wake after the dominant layer turned on outside a key event */
  kbdtimer_t timer_ = 0;
  kbdtimer_t wake_ = 0;
  bool       wake_armed_ = false;

  /* The dominant layer turned on before the deadline. The next event or report settles the wait */
  bool dominant_due_ = false;

  /* Set while this plugin re-emits a press, so its own hook lets it through */
  bool emitting_ = false;

  /* Set while the held-back press is replayed */
  bool replaying_ = false;

  /* Per position until its physical release. passed_ marks a press already let through, silenced_ a press that plays nothing */
  uint8_t passed_[POSITIONS / 8 + 1] = {0};
  uint8_t silenced_[POSITIONS / 8 + 1] = {0};

  /* A write is parsed here in full and saved only when every value is valid */
  static dominant_config_t staging_;

  static bool flagGet(const uint8_t *map, KeyAddr key_addr);
  static void flagSet(uint8_t *map, KeyAddr key_addr, bool value);

  bool configValid() const;
  uint16_t timeoutMs() const;
  uint8_t layerOf(uint8_t position) const;
  uint8_t layerAt(KeyAddr key_addr) const;

  static bool isLayerKey(Key key);
  static bool changesLayer(Key key);
  bool genuine(KeyAddr key_addr, uint8_t key_state) const;
  bool canArm(KeyAddr key_addr) const;
  bool predictsDominant(KeyAddr key_addr, Key key) const;
  bool settleIfDue();
  void resolve(Kind kind);

  void stageCurrent();
  void sendMap() const;
  bool readMap();
  bool readTimeout();
  static bool readLineEnd();
  static void drainLine();
  void cfgmem_config_save(const dominant_config_t *p_config);
};

static_assert(sizeof(DominantLayerDygma::dominant_config_t) == 64, "DominantLayer config block must stay 64 B");
static_assert(KeyAddr::upper_limit == DominantLayerDygma::POSITIONS, "The dominant map needs one entry per matrix position");

}
}

extern kaleidoscope::plugin::DominantLayerDygma DominantLayer;
