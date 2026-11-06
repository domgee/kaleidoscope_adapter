/* -*- mode: c++ -*-
 * Kaleidoscope-SwapHandsDygma -- QMK-style swap hands for Dygma keyboards
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

static_assert(ranges::SWAP_HANDS_MON == 54372, "Swap hands codes must match Bazecor");
static_assert(ranges::SWAP_HANDS_TAP_FIRST == 54376, "Swap hands codes must match Bazecor");
static_assert(ranges::SWAP_HANDS_TAP_LAST == 54631, "Swap hands codes must match Bazecor");
static_assert(ranges::SAFE_START == 54632, "Swap hands codes must match Bazecor");

/* Swap hands, as QMK swap hands. Mirror every key onto the other half while a swap key is active. The scanner router maps physical
 * addresses to logical ones before the core and any other plugin see an event. The plugin hook turns SH_T codes into their tap key
 * and consumes the other swap codes */
class SwapHandsDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t BUFFER_CAPACITY = 8;
  static constexpr uint8_t MAX_CONTROLS    = 4;
  static constexpr uint8_t MAX_TAPS        = 4;
  static constexpr uint8_t TAP_TOGGLE_TAPS = 5;

  EventHandlerResult onSetup();
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
  EventHandlerResult onFocusEvent(const char *command);

  /* Return the physical address that delivers at this logical address. KbdInterface reports it to the keyboard API */
  KeyAddr physicalFor(KeyAddr logical) const;

  /* Return true while a generated SH_T tap press is delivered at this address or still owned. Combos never takes it as a member */
  bool isTapAt(KeyAddr addr) const;

  /* Judge whether a key uses up SH_OS. SwapHandsOneShot calls it right before OneShot */
  void judgeOneShot(Key key, KeyAddr key_addr, uint8_t key_state);

  static bool isSwapCode(uint16_t raw) {
    return raw >= ranges::SWAP_HANDS_FIRST && raw <= ranges::SWAP_HANDS_LAST;
  }

  static KeyAddr mirror(KeyAddr addr) {
    return addr.isValid() ? KeyAddr(addr.row(), KeyAddr::cols - 1 - addr.col()) : addr;
  }

  /* Router callbacks, called by the key scanner through static trampolines */
  void scanBegin();
  void route(KeyAddr phys, uint8_t key_state);
  KeyAddr releaseTarget(KeyAddr phys) const;
  bool active() const;

 private:
  enum KeyState : uint8_t {
    NONE,       /* delivered at its own address, or not pressed */
    SWAPPED,    /* delivered at the mirror address until released */
    BUFFERED,   /* press waits behind an undecided SH_T or SH_TT */
    CONTROL,    /* a swap key, never delivered */
    DROPPED,    /* both target addresses were held, press and release dropped */
  };

  enum OsState : uint8_t {
    OS_OFF,
    OS_PRESSED,
    OS_USED,
    OS_ACTIVE,
  };

  struct entry_t {
    KeyAddr  phys;
    bool     release;
    uint32_t ms;
  };

  struct control_t {
    KeyAddr  phys;
    uint16_t code;        /* 0 = free or inert */
    bool     hold;        /* SH_T or SH_TT resolved as hold */
    bool     prev_swap;   /* swap state before an SH_TT hold */
  };

  struct tap_t {
    KeyAddr addr;
    uint8_t kc;
    bool    owned;             /* a Qukeys replay at addr still maps to kc */
    bool    release_pending;   /* release goes out at the next scanBegin */
  };

  uint8_t pstate_[KeyAddr::upper_limit] = {};
  uint8_t owned_count_ = 0;   /* physical keys not in NONE */

  /* Bit n = logical KeyAddr n */
  uint8_t occupied_[8] = {};   /* delivered by the router and not released */
  uint8_t os_mark_[8]  = {};   /* press delivered while SH_OS was armed */

  bool    swap_         = false;
  bool    pending_      = false;   /* an SH_T or SH_TT decision is open */
  bool    cancel_       = false;   /* an upgrade command cancels the open decision at the next scanBegin */
  bool    tap_deferred_ = false;   /* the decided tap goes out at the next scanBegin */
  uint8_t pending_ctrl_ = 0;
  uint32_t pending_ms_  = 0;
  uint8_t deferred_kc_  = 0;
  KeyAddr deferred_phys_;
  uint32_t clock_       = 0;       /* time of the event being routed, the original time during a buffer replay */

  entry_t buffer_[BUFFER_CAPACITY];
  uint8_t buffer_len_ = 0;

  control_t controls_[MAX_CONTROLS] = {};
  uint8_t   tt_order_[MAX_CONTROLS] = {};   /* SH_TT slots held as hold, oldest first */
  uint8_t   tt_order_len_ = 0;

  uint8_t  tt_taps_ = 0;
  KeyAddr  tt_addr_;
  uint32_t tt_release_ms_ = 0;

  tap_t   taps_[MAX_TAPS] = {};   /* emitted taps in emission order */
  uint8_t taps_len_ = 0;
  bool    emitted_this_scan_ = false;
  KeyAddr emitting_addr_;

  bool    inj_release_pending_ = false;
  uint8_t inj_kc_ = 0;

  OsState os_ = OS_OFF;

  kbdtimer_t decide_timer_ = 0;
  kbdtimer_t wake_timer_   = 0;
  kbdtimer_t os_timer_     = 0;

  static bool bitGet(const uint8_t *bits, KeyAddr addr);
  static void bitPut(uint8_t *bits, KeyAddr addr, bool value);

  uint8_t state(KeyAddr phys) const {
    return pstate_[phys.toInt()];
  }

  void setState(KeyAddr phys, uint8_t state);
  void send(KeyAddr logical, uint8_t key_state, Key key = Key_NoKey);

  void routePress(KeyAddr phys, uint32_t press_ms);
  void routeHeld(KeyAddr phys, uint8_t key_state);
  void routeRelease(KeyAddr phys, uint8_t key_state);
  void append(KeyAddr phys, uint32_t ms, bool release);
  void expireByClock();

  int8_t findControl(KeyAddr phys) const;
  void freeControls(KeyAddr phys);
  void controlPress(KeyAddr phys, uint16_t code, uint32_t press_ms);
  void controlRelease(KeyAddr phys);
  void releaseTapToggleHold(uint8_t idx);

  void startPending(uint8_t idx, uint32_t press_ms);
  void resolveHold();
  void resolveTap(uint8_t idx);
  void finishDeferredTap();
  void cancelPending();
  void replayBuffer();

  void emitTap(KeyAddr phys, uint8_t kc);
  void releaseInjected();

  void judgeOneShotKey(Key key);
  void endOneShot();
};

/* Judge SH_OS on the resolved key, registered right before OneShot so it sees the keys OneShot sees */
class SwapHandsOneShotDygma : public kaleidoscope::Plugin {
 public:
  EventHandlerResult onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state);
};

}
}

extern kaleidoscope::plugin::SwapHandsDygma SwapHands;
extern kaleidoscope::plugin::SwapHandsOneShotDygma SwapHandsOneShot;
