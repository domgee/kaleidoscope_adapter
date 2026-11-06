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

#include "SwapHandsDygma.h"

#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"
#include "kaleidoscope/layers.h"
#include "KeyboardioHID.h"
#include "KeyRoleManager.h"
#include "OneShotDygma.h"
#include "QukeysDygma.h"

namespace kaleidoscope {
namespace plugin {

typedef kaleidoscope::Device::KeyScanner Scanner;

static void routerScanBegin()
{
  ::SwapHands.scanBegin();
}

static void routerRoute(KeyAddr phys, uint8_t key_state)
{
  ::SwapHands.route(phys, key_state);
}

static KeyAddr routerReleaseTarget(KeyAddr phys)
{
  return ::SwapHands.releaseTarget(phys);
}

static bool routerActive()
{
  return ::SwapHands.active();
}

static const Scanner::Router router = { routerScanBegin, routerRoute, routerReleaseTarget, routerActive };

/* ------------------------------------------------------------------------- */
/* Setup and router callbacks                                                */
/* ------------------------------------------------------------------------- */

EventHandlerResult SwapHandsDygma::onSetup()
{
  Scanner::setRouter(&router);

  return EventHandlerResult::OK;
}

bool SwapHandsDygma::active() const
{
  return swap_ || pending_ || os_ != OS_OFF || taps_len_ != 0 || inj_release_pending_ || owned_count_ != 0;
}

KeyAddr SwapHandsDygma::releaseTarget(KeyAddr phys) const
{
  if (!phys.isValid()) {
    return KeyAddr();
  }

  switch (state(phys)) {
  case NONE:
    return phys;
  case SWAPPED:
    return mirror(phys);
  default:
    return KeyAddr();
  }
}

void SwapHandsDygma::route(KeyAddr phys, uint8_t key_state)
{
  if (!phys.isValid()) {
    Scanner::deliver(phys, key_state);
    return;
  }

  clock_ = Runtime.millisAtCycleStart();

  if (keyToggledOn(key_state)) {
    routePress(phys, clock_);
  } else if (keyToggledOff(key_state)) {
    routeRelease(phys, key_state);
  } else {
    routeHeld(phys, key_state);
  }
}

/* Run once per scan before any event. Release last scan's taps, then finish deferred work and deadlines */
void SwapHandsDygma::scanBegin()
{
  clock_ = Runtime.millisAtCycleStart();

  if (taps_len_ != 0 || inj_release_pending_ || tap_deferred_) {
    kbdtimer_check(&wake_timer_);
  }

  emitted_this_scan_ = false;

  if (taps_len_ != 0) {
    for (uint8_t i = 0; i < taps_len_; ++i) {
      if (taps_[i].release_pending) {
        taps_[i].release_pending = false;
        send(taps_[i].addr, WAS_PRESSED);
      }
    }

    uint8_t kept = 0;

    for (uint8_t i = 0; i < taps_len_; ++i) {
      /* Drop ownership when Qukeys holds nothing at the address any more, so no replay can come */
      if (taps_[i].owned && !::qukeys.hasQueuedEvent(taps_[i].addr)) {
        taps_[i].owned = false;
      }

      if (taps_[i].owned || taps_[i].release_pending) {
        taps_[kept++] = taps_[i];
      }
    }

    taps_len_ = kept;
  }

  if (inj_release_pending_) {
    releaseInjected();
  }

  if (pending_ && cancel_) {
    cancelPending();
  }

  cancel_ = false;

  if (pending_ && tap_deferred_) {
    finishDeferredTap();
  }

  if (pending_ && !tap_deferred_ && kbdtimer_check(&decide_timer_)) {
    resolveHold();
  }

  if (os_ == OS_ACTIVE && kbdtimer_check(&os_timer_)) {
    endOneShot();
  }
}

/* ------------------------------------------------------------------------- */
/* State helpers                                                             */
/* ------------------------------------------------------------------------- */

bool SwapHandsDygma::bitGet(const uint8_t *bits, KeyAddr addr)
{
  if (!addr.isValid()) {
    return false;
  }

  uint8_t offset = addr.toInt();

  return (bits[offset >> 3] >> (offset & 7)) & 1;
}

void SwapHandsDygma::bitPut(uint8_t *bits, KeyAddr addr, bool value)
{
  if (!addr.isValid()) {
    return;
  }

  uint8_t offset = addr.toInt();

  if (value) {
    bits[offset >> 3] |= (1 << (offset & 7));
  } else {
    bits[offset >> 3] &= ~(1 << (offset & 7));
  }
}

void SwapHandsDygma::setState(KeyAddr phys, uint8_t new_state)
{
  uint8_t &current = pstate_[phys.toInt()];

  if (current == NONE && new_state != NONE) {
    owned_count_++;
  } else if (current != NONE && new_state == NONE) {
    owned_count_--;
  }

  current = new_state;
}

void SwapHandsDygma::send(KeyAddr logical, uint8_t key_state, Key key)
{
  if (keyToggledOn(key_state)) {
    bitPut(occupied_, logical, true);

    if (os_ != OS_OFF) {
      bitPut(os_mark_, logical, true);
    }
  } else if (keyToggledOff(key_state)) {
    bitPut(occupied_, logical, false);
  }

  Scanner::deliver(logical, key_state, key);
}

/* ------------------------------------------------------------------------- */
/* Routing                                                                   */
/* ------------------------------------------------------------------------- */

void SwapHandsDygma::append(KeyAddr phys, uint32_t ms, bool release)
{
  buffer_[buffer_len_].phys = phys;
  buffer_[buffer_len_].ms = ms;
  buffer_[buffer_len_].release = release;
  buffer_len_++;
}

/* Resolve an open decision as hold when the event being routed comes after its hold timeout, also inside a buffer replay */
void SwapHandsDygma::expireByClock()
{
  if (pending_ && !tap_deferred_ && clock_ - pending_ms_ >= ::qukeys.holdTimeout()) {
    resolveHold();
  }
}

void SwapHandsDygma::routePress(KeyAddr phys, uint32_t press_ms)
{
  uint8_t st = state(phys);

  /* Clean up after a missed release */
  if (st == SWAPPED) {
    send(mirror(phys), WAS_PRESSED);
  } else if (st == CONTROL) {
    freeControls(phys);
  }

  if (st != NONE && st != BUFFERED) {
    setState(phys, NONE);
  }

  if (tt_taps_ != 0 && phys != tt_addr_) {
    tt_taps_ = 0;
  }

  expireByClock();

  while (pending_) {
    if (buffer_len_ < BUFFER_CAPACITY) {
      append(phys, press_ms, false);
      setState(phys, BUFFERED);
      return;
    }

    /* A full buffer forces the decision */
    if (tap_deferred_) {
      finishDeferredTap();
    } else {
      resolveHold();
    }
  }

  uint16_t raw = Layer.lookupOnActiveLayer(phys).getRaw();

  if (isSwapCode(raw)) {
    ::qukeys.notePhysicalPress(phys);
    setState(phys, CONTROL);
    controlPress(phys, raw, press_ms);
    return;
  }

  KeyAddr target = (swap_ || os_ != OS_OFF) ? mirror(phys) : phys;

  /* Land on the other address when the target is held, drop the press when both are */
  if (bitGet(occupied_, target)) {
    target = (target == phys) ? mirror(phys) : phys;

    if (bitGet(occupied_, target)) {
      setState(phys, DROPPED);
      ::qukeys.notePhysicalPress(phys);
      return;
    }
  }

  if (target != phys) {
    setState(phys, SWAPPED);
  }

  send(target, IS_PRESSED);
}

void SwapHandsDygma::routeHeld(KeyAddr phys, uint8_t key_state)
{
  uint8_t st = state(phys);

  if (st == BUFFERED || st == CONTROL || st == DROPPED) {
    return;
  }

  send(st == SWAPPED ? mirror(phys) : phys, key_state);
}

void SwapHandsDygma::routeRelease(KeyAddr phys, uint8_t key_state)
{
  expireByClock();

  /* Never drop a release while its press is still buffered. Pressing and releasing a key inside the decision makes it a hold */
  while (state(phys) == BUFFERED && pending_) {
    if (tap_deferred_) {
      if (buffer_len_ < BUFFER_CAPACITY) {
        append(phys, clock_, true);
        return;
      }

      finishDeferredTap();
    } else {
      resolveHold();
    }
  }

  uint8_t st = state(phys);

  if (st == BUFFERED || st == DROPPED) {
    setState(phys, NONE);
    return;
  }

  if (st == CONTROL) {
    controlRelease(phys);
    setState(phys, NONE);
    return;
  }

  send(st == SWAPPED ? mirror(phys) : phys, key_state);
  setState(phys, NONE);
}

/* ------------------------------------------------------------------------- */
/* Swap keys                                                                 */
/* ------------------------------------------------------------------------- */

int8_t SwapHandsDygma::findControl(KeyAddr phys) const
{
  for (uint8_t i = 0; i < MAX_CONTROLS; ++i) {
    if (controls_[i].code != 0 && controls_[i].phys == phys) {
      return i;
    }
  }

  return -1;
}

void SwapHandsDygma::freeControls(KeyAddr phys)
{
  int8_t idx;

  while ((idx = findControl(phys)) >= 0) {
    controls_[idx].code = 0;

    for (uint8_t i = 0; i < tt_order_len_; ++i) {
      if (tt_order_[i] == idx) {
        for (uint8_t j = i + 1; j < tt_order_len_; ++j) {
          tt_order_[j - 1] = tt_order_[j];
        }

        tt_order_len_--;
        break;
      }
    }
  }
}

void SwapHandsDygma::controlPress(KeyAddr phys, uint16_t code, uint32_t press_ms)
{
  int8_t idx = -1;

  for (uint8_t i = 0; i < MAX_CONTROLS; ++i) {
    if (controls_[i].code == 0) {
      idx = i;
      break;
    }
  }

  /* Leave the key inert when every slot is taken */
  if (idx < 0) {
    return;
  }

  controls_[idx].phys = phys;
  controls_[idx].code = code;
  controls_[idx].hold = false;
  controls_[idx].prev_swap = false;

  if (code == ranges::SWAP_HANDS_MON) {
    swap_ = true;
  } else if (code == ranges::SWAP_HANDS_TOGGLE) {
    swap_ = !swap_;
  } else if (code == ranges::SWAP_HANDS_ONESHOT) {
    os_ = OS_PRESSED;
  } else if (code == ranges::SWAP_HANDS_TAP_TOGGLE) {
    if (tt_addr_ != phys || press_ms - tt_release_ms_ >= ::qukeys.holdTimeout()) {
      tt_taps_ = 0;
    }

    tt_addr_ = phys;
    startPending(idx, press_ms);
  } else {
    startPending(idx, press_ms);
  }
}

void SwapHandsDygma::controlRelease(KeyAddr phys)
{
  int8_t idx = findControl(phys);

  if (idx < 0) {
    return;
  }

  uint16_t code = controls_[idx].code;

  if (code == ranges::SWAP_HANDS_MON) {
    swap_ = false;
  } else if (code == ranges::SWAP_HANDS_ONESHOT) {
    if (os_ == OS_PRESSED) {
      kbdtimer_set_ms(&os_timer_, OneShot::one_shot_time_out);
      os_ = OS_ACTIVE;
    } else if (os_ == OS_USED) {
      endOneShot();
    }
  } else if (code != ranges::SWAP_HANDS_TOGGLE) {
    if (pending_ && pending_ctrl_ == idx && !tap_deferred_) {
      resolveTap(idx);
    } else if (controls_[idx].hold) {
      if (code == ranges::SWAP_HANDS_TAP_TOGGLE) {
        releaseTapToggleHold(idx);
      } else {
        swap_ = false;
      }
    }
  }

  controls_[idx].code = 0;
}

/* End an SH_TT hold. The newest one restores its own pre-hold state, an older one flips the state and hands its pre-hold state on */
void SwapHandsDygma::releaseTapToggleHold(uint8_t idx)
{
  for (uint8_t i = 0; i < tt_order_len_; ++i) {
    if (tt_order_[i] != idx) {
      continue;
    }

    if (i == tt_order_len_ - 1) {
      swap_ = controls_[idx].prev_swap;
    } else {
      swap_ = !swap_;
      controls_[tt_order_[i + 1]].prev_swap = controls_[idx].prev_swap;
    }

    for (uint8_t j = i + 1; j < tt_order_len_; ++j) {
      tt_order_[j - 1] = tt_order_[j];
    }

    tt_order_len_--;
    return;
  }
}

/* ------------------------------------------------------------------------- */
/* Tap or hold decision for SH_T and SH_TT                                   */
/* ------------------------------------------------------------------------- */

void SwapHandsDygma::startPending(uint8_t idx, uint32_t press_ms)
{
  pending_ = true;
  tap_deferred_ = false;
  pending_ctrl_ = idx;
  pending_ms_ = press_ms;
  buffer_len_ = 0;

  uint16_t hold_timeout = ::qukeys.holdTimeout();

  if (clock_ - press_ms >= hold_timeout) {
    resolveHold();
    return;
  }

  /* Count the deadline from the original press, also for a replayed press */
  uint32_t elapsed = Runtime.millisAtCycleStart() - press_ms;
  uint32_t remaining = elapsed < hold_timeout ? hold_timeout - elapsed : 1;

  kbdtimer_set_ms(&decide_timer_, remaining);
}

void SwapHandsDygma::resolveHold()
{
  if (!pending_ || tap_deferred_) {
    return;
  }

  pending_ = false;

  control_t &control = controls_[pending_ctrl_];
  control.hold = true;

  if (control.code == ranges::SWAP_HANDS_TAP_TOGGLE) {
    control.prev_swap = swap_;
    swap_ = !swap_;
    tt_taps_ = 0;

    if (tt_order_len_ < MAX_CONTROLS) {
      tt_order_[tt_order_len_++] = pending_ctrl_;
    }
  } else if (control.code >= ranges::SWAP_HANDS_TAP_FIRST && control.code <= ranges::SWAP_HANDS_TAP_LAST) {
    swap_ = true;
  }

  replayBuffer();
}

void SwapHandsDygma::resolveTap(uint8_t idx)
{
  if (!pending_) {
    return;
  }

  uint16_t code = controls_[idx].code;

  if (code == ranges::SWAP_HANDS_TAP_TOGGLE) {
    tt_taps_++;
    tt_release_ms_ = clock_;

    if (tt_taps_ >= TAP_TOGGLE_TAPS) {
      swap_ = !swap_;
      tt_taps_ = 0;
    }

    pending_ = false;
    replayBuffer();
    return;
  }

  uint8_t kc = code - ranges::SWAP_HANDS_TAP_FIRST;

  if (kc == 0) {
    pending_ = false;
    replayBuffer();
    return;
  }

  /* Never press and release a tap in one scan. A second tap in this scan goes out at the next scanBegin, the buffer keeps filling */
  if (emitted_this_scan_) {
    kbdtimer_set_ms(&wake_timer_, 1);
    tap_deferred_ = true;
    deferred_kc_ = kc;
    deferred_phys_ = controls_[idx].phys;
    return;
  }

  pending_ = false;
  emitTap(controls_[idx].phys, kc);
  replayBuffer();
}

void SwapHandsDygma::finishDeferredTap()
{
  pending_ = false;
  tap_deferred_ = false;
  emitTap(deferred_phys_, deferred_kc_);
  replayBuffer();
}

/* Close the decision for an upgrade command. Buffered swap keys stay inert until released, other keys replay unswapped */
void SwapHandsDygma::cancelPending()
{
  pending_ = false;
  tap_deferred_ = false;
  controls_[pending_ctrl_].code = 0;

  entry_t entries[BUFFER_CAPACITY];
  uint8_t count = buffer_len_;

  memcpy(entries, buffer_, sizeof(entry_t) * count);
  buffer_len_ = 0;

  uint32_t saved_clock = clock_;

  for (uint8_t i = 0; i < count; ++i) {
    KeyAddr phys = entries[i].phys;

    clock_ = entries[i].ms;

    if (entries[i].release) {
      routeRelease(phys, WAS_PRESSED);
    } else if (isSwapCode(Layer.lookupOnActiveLayer(phys).getRaw())) {
      setState(phys, CONTROL);
    } else {
      setState(phys, NONE);
      routePress(phys, entries[i].ms);
    }
  }

  clock_ = saved_clock;
}

/* Route every buffered event once, in order, at its original time. A replayed SH_T or SH_TT opens a new decision that buffers
 * the entries after it */
void SwapHandsDygma::replayBuffer()
{
  if (buffer_len_ == 0) {
    return;
  }

  entry_t entries[BUFFER_CAPACITY];
  uint8_t count = buffer_len_;

  memcpy(entries, buffer_, sizeof(entry_t) * count);
  buffer_len_ = 0;

  uint32_t saved_clock = clock_;

  for (uint8_t i = 0; i < count; ++i) {
    KeyAddr phys = entries[i].phys;

    clock_ = entries[i].ms;

    if (entries[i].release) {
      routeRelease(phys, WAS_PRESSED);
    } else {
      if (state(phys) == BUFFERED) {
        setState(phys, NONE);
      }

      routePress(phys, entries[i].ms);
    }
  }

  clock_ = saved_clock;
}

/* ------------------------------------------------------------------------- */
/* SH_T taps                                                                 */
/* ------------------------------------------------------------------------- */

/* Press the tap key at the SH_T key's address. Its release goes out at the next scanBegin, so the host sees the press in a report */
void SwapHandsDygma::emitTap(KeyAddr phys, uint8_t kc)
{
  emitted_this_scan_ = true;
  kbdtimer_set_ms(&wake_timer_, 1);

  if (!bitGet(occupied_, phys) && taps_len_ < MAX_TAPS) {
    uint8_t i = taps_len_++;

    taps_[i].addr = phys;
    taps_[i].kc = kc;
    taps_[i].owned = true;
    taps_[i].release_pending = true;

    emitting_addr_ = phys;
    send(phys, IS_PRESSED, Key(kc, KEY_FLAGS));
    emitting_addr_ = KeyAddr();

    /* Keep ownership only while Qukeys holds the press for a later replay */
    if (!::qukeys.hasQueuedEvent(phys)) {
      taps_[i].owned = false;
    }

    return;
  }

  /* Fall back to an injected tap when the mirror key holds this address or the table is full */
  if (inj_release_pending_) {
    releaseInjected();
  }

  inj_kc_ = kc;
  inj_release_pending_ = true;
  handleKeyswitchEvent(Key(kc, KEY_FLAGS), UnknownKeyswitchLocation, IS_PRESSED | INJECTED);
  judgeOneShotKey(Key(kc, KEY_FLAGS));
}

/* Release the injected tap and use up OneShot keys, which ignore injected events */
void SwapHandsDygma::releaseInjected()
{
  Key key(inj_kc_, KEY_FLAGS);

  inj_release_pending_ = false;
  handleKeyswitchEvent(key, UnknownKeyswitchLocation, WAS_PRESSED | INJECTED);
  OneShot::consumeOnKeyPress(key);
}

/* ------------------------------------------------------------------------- */
/* Plugin hooks                                                              */
/* ------------------------------------------------------------------------- */

EventHandlerResult SwapHandsDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  /* Give a Qukeys replay of an emitted tap its tap key, also after a layer change */
  if (taps_len_ != 0 && keyToggledOn(key_state) && !(key_state & INJECTED) && keyRoleManager.isReplayingQueuedEvent()) {
    for (uint8_t i = 0; i < taps_len_; ++i) {
      if (taps_[i].owned && taps_[i].addr == key_addr) {
        taps_[i].owned = false;
        mapped_key = Key(taps_[i].kc, KEY_FLAGS);
        Layer.updateLiveCompositeKeymap(key_addr, mapped_key);
        return EventHandlerResult::OK;
      }
    }
  }

  uint16_t raw = mapped_key.getRaw();

  if (!isSwapCode(raw)) {
    return EventHandlerResult::OK;
  }

  /* A key mirrored onto an SH_T cell types its tap key. Swap codes act only from the router, so consume every other one */
  uint8_t kc = raw >= ranges::SWAP_HANDS_TAP_FIRST ? raw - ranges::SWAP_HANDS_TAP_FIRST : 0;

  if (kc == 0 || (key_state & INJECTED) || !key_addr.isValid()) {
    return EventHandlerResult::EVENT_CONSUMED;
  }

  mapped_key = Key(kc, KEY_FLAGS);

  if (keyToggledOn(key_state)) {
    Layer.updateLiveCompositeKeymap(key_addr, mapped_key);
  }

  return EventHandlerResult::OK;
}

EventHandlerResult SwapHandsDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, "swaphands.state")) {
    return EventHandlerResult::OK;
  }

  /* Clear swap and SH_OS for the flash approval, which checks the physical top-left key */
  if (strncmp_P(command, "upgrade.", 8) == 0) {
    swap_ = false;
    endOneShot();
    tt_taps_ = 0;

    for (uint8_t i = 0; i < MAX_CONTROLS; ++i) {
      controls_[i].code = 0;
    }

    tt_order_len_ = 0;

    if (pending_) {
      cancel_ = true;
    }

    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "swaphands.state") != 0) {
    return EventHandlerResult::OK;
  }

  /* Report only. Bazecor offers the swap keys when help lists this command */
  ::Focus.send((uint8_t)swap_, (uint8_t)os_, (uint8_t)pending_);
  return EventHandlerResult::EVENT_CONSUMED;
}

KeyAddr SwapHandsDygma::physicalFor(KeyAddr logical) const
{
  KeyAddr phys = mirror(logical);

  if (logical.isValid() && state(phys) == SWAPPED) {
    return phys;
  }

  return logical;
}

bool SwapHandsDygma::isTapAt(KeyAddr addr) const
{
  if (addr == emitting_addr_ && addr.isValid()) {
    return true;
  }

  for (uint8_t i = 0; i < taps_len_; ++i) {
    if (taps_[i].owned && taps_[i].addr == addr) {
      return true;
    }
  }

  return false;
}

/* ------------------------------------------------------------------------- */
/* SH_OS                                                                     */
/* ------------------------------------------------------------------------- */

/* Judge only presses the router delivered while SH_OS was armed, once each */
void SwapHandsDygma::judgeOneShot(Key key, KeyAddr key_addr, uint8_t key_state)
{
  if (os_ == OS_OFF || !bitGet(os_mark_, key_addr)) {
    return;
  }

  if (keyToggledOff(key_state)) {
    bitPut(os_mark_, key_addr, false);
  } else if (keyToggledOn(key_state)) {
    bitPut(os_mark_, key_addr, false);
    judgeOneShotKey(key);
  }
}

/* Keep SH_OS for modifiers, layer keys, OneShot keys, swap keys and unresolved dual-function keys, as OneShot does */
void SwapHandsDygma::judgeOneShotKey(Key key)
{
  if (os_ == OS_OFF) {
    return;
  }

  uint16_t raw = key.getRaw();
  uint8_t flags = key.getFlags();
  uint8_t keycode = key.getKeyCode();

  if ((flags & (SYNTHETIC | RESERVED)) == 0 && keycode >= HID_KEYBOARD_FIRST_MODIFIER && keycode <= HID_KEYBOARD_LAST_MODIFIER) {
    return;
  }

  if (flags == (SYNTHETIC | SWITCH_TO_KEYMAP)) {
    return;
  }

  if ((raw >= ranges::OS_FIRST && raw <= ranges::OS_LAST) || isSwapCode(raw)) {
    return;
  }

  if ((raw >= ranges::DU_FIRST && raw <= ranges::DU_LAST) || (raw >= ranges::DYNAMIC_SUPER_FIRST && raw <= ranges::DYNAMIC_SUPER_LAST)) {
    return;
  }

  if (os_ == OS_PRESSED) {
    os_ = OS_USED;
  } else if (os_ == OS_ACTIVE) {
    endOneShot();
  }
}

void SwapHandsDygma::endOneShot()
{
  os_ = OS_OFF;
  memset(os_mark_, 0, sizeof(os_mark_));
}

EventHandlerResult SwapHandsOneShotDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  ::SwapHands.judgeOneShot(mapped_key, key_addr, key_state);

  return EventHandlerResult::OK;
}

}
}

kaleidoscope::plugin::SwapHandsDygma SwapHands;
kaleidoscope::plugin::SwapHandsOneShotDygma SwapHandsOneShot;
