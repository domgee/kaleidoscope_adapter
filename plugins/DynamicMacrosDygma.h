/* DynamicMacros - Dynamic macro support for Kaleidoscope.
 * Copyright (C) 2019  Keyboard.io, Inc.
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
#include <Kaleidoscope-Ranges.h>

#include "kaleidoscope/plugin/Macros/MacroSteps.h"

#include "UnicodeHexInput.h"

#define DM(n) Key(kaleidoscope::ranges::DYNAMIC_MACRO_FIRST + n)

namespace kaleidoscope {
namespace plugin {

/* Non-blocking macro player. Up to MAX_RUNS macros play alongside each other, one output unit per cycle, with waits on a timer */
class DynamicMacros : public kaleidoscope::Plugin {
 public:
  typedef struct PACK
  {
      uint8_t memory[ APP_MACROS_MEMORY_SIZE ];
  } macros_config_t;

  /* Steps after MACRO_ACTION_STEP_TAP_CODE_SEQUENCE */
  static constexpr uint8_t STEP_OPTIONS = 14;
  static constexpr uint8_t STEP_RELEASE = 15;
  static constexpr uint8_t STEP_UNICODE = 16;

  /* Option byte of STEP_OPTIONS. A macro without the step has OPT_VALID only */
  static constexpr uint8_t OPT_VALID           = 0x80;
  static constexpr uint8_t OPT_MODE            = 0x03;
  static constexpr uint8_t OPT_IGNORE_MODS     = 0x04;
  static constexpr uint8_t OPT_OTHER_KEYS_STOP = 0x08;
  static constexpr uint8_t OPT_KNOWN           = 0x8F;

  static constexpr uint8_t MODE_ONCE   = 0;
  static constexpr uint8_t MODE_REPEAT = 1;
  static constexpr uint8_t MODE_TOGGLE = 2;

  static constexpr uint8_t  MAX_MACROS         = 128;
  static constexpr uint8_t  MAX_RUNS           = 4;
  static constexpr uint8_t  MAX_HELD           = 8;
  static constexpr uint8_t  STEPS_PER_CYCLE    = 32;
  static constexpr uint8_t  NONE               = 0xFF;
  static constexpr uint16_t BACKPRESSURE_BYTES = 70;   /* about two keyboard reports in the HID FIFO */

  enum : uint8_t { ERR_NONE, ERR_POOL_FULL, ERR_BAD_ID, ERR_CYCLE, ERR_HELD_FULL, ERR_BUSY, ERR_OUTPUT };

  DynamicMacros(void) {}

  EventHandlerResult onKeyswitchEvent(Key &mappedKey, KeyAddr key_addr, uint8_t keyState);
  EventHandlerResult onFocusEvent(const char *command);
  EventHandlerResult beforeEachCycle();
  EventHandlerResult beforeReportingState();

  static void reserve_storage( void );

  /* Stop the running macros that other keys stop. Call it for every genuine physical press, next to qukeys.notePhysicalPress */
  void notePhysicalPress(KeyAddr key_addr);

  /* Return the trigger address of the macro injecting a key now, invalid when none injects or the macro has no address */
  KeyAddr injectingFor() const;

  /* Return true while a macro injects a Unicode hex input key, which history and CapsWord ignore */
  bool injectingProtocol() const;

 private:
  enum : uint8_t { RUN_FREE, RUN_PLAYING, RUN_WAITING, RUN_PAUSED, RUN_CHILD };
  enum : uint8_t { SUB_NONE, SUB_TAP_RELEASE, SUB_SEQ, SUB_SEQ_RELEASE, SUB_CODESEQ, SUB_CODESEQ_RELEASE, SUB_UNICODE };
  enum : uint8_t { INJ_OK, INJ_ALREADY, INJ_FAILED };

  /* Run.flags */
  static constexpr uint8_t F_RELEASED    = 0x01;   /* the trigger is up, or the run has no trigger */
  static constexpr uint8_t F_EXPLICIT    = 0x02;   /* inside an explicit-report block */
  static constexpr uint8_t F_MARKER_SEEN = 0x04;   /* passed the release marker */
  static constexpr uint8_t F_FRESH       = 0x08;   /* started in this cycle */
  static constexpr uint8_t F_REPLAY      = 0x10;   /* play once more after END */
  static constexpr uint8_t F_STOPPING    = 0x20;   /* root only, finish the tree */
  static constexpr uint8_t F_ITER_OUTPUT = 0x40;   /* this iteration sent a key or waited */
  static constexpr uint8_t F_ITER_DONE   = 0x80;   /* one full iteration played */

  /* Run.xflags */
  static constexpr uint8_t X_ONESHOT = 0x01;   /* OneShot was active at root start */

  /* Held.kind */
  static constexpr uint8_t HELD_TEMP     = 0x01;   /* tap, sequence or Unicode press until its release */
  static constexpr uint8_t HELD_PROTOCOL = 0x02;   /* Unicode hex input key */

  struct Run {
    uint32_t due_ms;    /* Runtime.millisAtCycleStart() clock */
    uint16_t pos;
    uint16_t body;      /* first step after the option step */
    Key      sub_key;   /* key of the tap or sequence element waiting for its release */
    KeyAddr  trigger;   /* invalid for Focus starts and children */
    uint8_t  id;
    uint8_t  state;
    uint8_t  opts;
    uint8_t  flags;
    uint8_t  sub;
    uint8_t  parent;
    uint8_t  xflags;
  };

  struct Held {
    Key     key;
    uint8_t owner;
    uint8_t kind;
  };

  /* A press that stopped macros. Its resolved press of one of these ids does not start the macro again */
  struct Token {
    KeyAddr addr;
    uint8_t count;
    uint8_t ids[MAX_RUNS];
  };

  static const macros_config_t * p_macros_config;

  static uint16_t map_[MAX_MACROS + 1];
  static uint8_t count_;
  static void updateDynamicMacroCache(void);
  static uint16_t stepLength(uint16_t pos, uint8_t type);
  static void optionsOf(uint8_t id, uint8_t &opts, uint16_t &body);

  Run     runs_[MAX_RUNS];
  Held    held_[MAX_HELD];
  Token   tokens_[MAX_RUNS];
  uint8_t held_count_     = 0;
  uint8_t active_count_   = 0;
  uint8_t token_count_    = 0;
  uint8_t rr_             = 0;
  uint8_t last_error_     = ERR_NONE;
  uint8_t injecting_run_  = NONE;
  bool    injecting_protocol_ = false;
  uint8_t uni_owner_      = NONE;   /* run that plays a Unicode scalar, the others wait */
  uint8_t uni_closing_    = NONE;   /* run whose scalar ends in this cycle's report, which must have Left Alt up */
  unicode_hex::Sequence uni_;
  uint8_t mask_remove_last_ = 0;   /* macro modifier filter last set in KeyboardioHID */
  uint8_t mask_add_last_    = 0;
  uint8_t flag_hold_      = 0;   /* Key flag modifiers kept for held keys after an injected release */
  bool    mask_reset_     = false;   /* a key press reset Kaleidoscope's modifier flag mask in this cycle */
  bool    timer_pending_  = false;
  kbdtimer_t timer_       = 0;
  uint32_t rng_           = 0;

  void onMacroPress(uint8_t id, KeyAddr key_addr, uint8_t key_state);
  void onMacroRelease(uint8_t id, KeyAddr key_addr);
  void onTriggerReleased(uint8_t i);
  void startRoot(uint8_t id, KeyAddr key_addr);
  bool startChild(uint8_t i, uint8_t id);
  void requestStop(uint8_t root);
  void wakeSoon();

  uint8_t findRun(uint8_t id) const;
  uint8_t rootOf(uint8_t i) const;
  uint8_t modeOf(const Run &r) const;

  void addToken(KeyAddr key_addr, const uint8_t *ids, uint8_t count);
  void dropToken(uint8_t t);

  uint8_t injectPress(uint8_t i, Key key, uint8_t kind);
  void releaseTemp(uint8_t i, Key key);
  void releasePersistent(uint8_t i, Key key);
  void releaseEvent(uint8_t i, Key key, uint8_t kind);
  void releaseHeldOf(uint8_t i);
  void removeHeld(uint8_t h);
  void failStop(uint8_t i);
  void freeRun(uint8_t i);

  bool otherReleasePending(uint8_t i) const;
  bool step(uint8_t i, uint32_t now);
  void endOfMacro(uint8_t i, uint32_t now);
  void finishTree(uint8_t i);
  void finishRoot(uint8_t i, bool at_end);
  void finishStops();
  bool pickAndStep(uint32_t now);
  void armTimer(uint32_t now);
  void abortAll(uint8_t err);
  void goIdle();
  void applyMask();
  uint16_t randomDelay(uint16_t min_ms, uint16_t max_ms);

  uint8_t heldFlagMods() const;

  static void cfgmem_macro_mem_save( uint16_t mem_pos, uint8_t * p_data, uint16_t data_len );
};

}
}

extern kaleidoscope::plugin::DynamicMacros DynamicMacros;
