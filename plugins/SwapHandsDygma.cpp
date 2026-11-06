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
  return swap_ || pending_ || draining_ || os_ != OS_OFF || taps_len_ != 0 || inj_release_pending_ || owned_count_ != 0;
}

KeyAddr SwapHandsDygma::releaseTarget(KeyAddr phys) const
{
  /* While draining a release queues behind the buffer, so its logical bit stays until it is replayed */
  if (!phys.isValid() || draining_) {
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

/* Run once per scan before any event. Release last scan's taps, then finish deadlines and send the next buffered event */
void SwapHandsDygma::scanBegin()
{
  clock_ = Runtime.millisAtCycleStart();

  if (super_holds_ != 0) {
    checkSuperHolds();
  }

  if (taps_len_ != 0 || inj_release_pending_ || draining_) {
    kbdtimer_check(&wake_timer_);
  }

  sent_this_scan_ = false;

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

  if (cancel_ && (pending_ || draining_)) {
    cancelPending();
  }

  cancel_ = false;

  /* End the drain one scan after its last event, so a new press never shares a report with it */
  if (draining_ && !tap_deferred_ && buffer_len_ == 0) {
    draining_ = false;
  }

  if (pending_ && kbdtimer_check(&decide_timer_)) {
    resolveHold();
  }

  if (draining_ && !sent_this_scan_) {
    drainOne();
  }

  if (buffer_len_ != 0) {
    restoreHeld();
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

void SwapHandsDygma::append(const entry_t &entry)
{
  buffer_[buffer_len_++] = entry;
}

void SwapHandsDygma::appendPress(KeyAddr phys, uint32_t ms)
{
  entry_t entry;

  entry.phys = phys;
  entry.release = false;
  entry.snap = BY_STATE;
  entry.ms = ms;
  append(entry);
  setState(phys, BUFFERED);
}

void SwapHandsDygma::appendRelease(KeyAddr phys, uint32_t ms, uint8_t snap, KeyAddr target)
{
  entry_t entry;

  entry.phys = phys;
  entry.target = target;
  entry.release = true;
  entry.snap = snap;
  entry.ms = ms;
  append(entry);
}

/* Free a buffer slot. A full buffer forces an open decision as hold, then sends one more buffered event in this scan */
void SwapHandsDygma::makeRoom()
{
  if (pending_) {
    resolveHold();
  } else {
    drainOne();
  }
}

/* Resolve an open decision as hold when the event being routed comes after its hold timeout, also for a refed buffer */
void SwapHandsDygma::expireByClock()
{
  if (pending_ && clock_ - pending_ms_ >= ::qukeys.holdTimeout(controls_[pending_ctrl_].phys)) {
    resolveHold();
  }
}

bool SwapHandsDygma::isSwapCell(KeyAddr phys) const
{
  return isSwapCode(Layer.lookupOnActiveLayer(phys).getRaw());
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

  while (pending_ || draining_) {
    if (buffer_len_ < BUFFER_CAPACITY) {
      appendPress(phys, press_ms);
      return;
    }

    makeRoom();
  }

  deliverPress(phys, press_ms);
}

void SwapHandsDygma::deliverPress(KeyAddr phys, uint32_t press_ms)
{
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

  /* Never drop a release while its press is still buffered. Pressing and releasing a key inside the decision makes it a hold,
   * except for a swap key */
  while (state(phys) == BUFFERED && (pending_ || draining_)) {
    if (pending_ && !isSwapCell(phys)) {
      resolveHold();
    } else if (buffer_len_ < BUFFER_CAPACITY) {
      appendRelease(phys, clock_, BY_STATE, KeyAddr());
      return;
    } else {
      makeRoom();
    }
  }

  /* Queue the release of a delivered key behind the drain, and keep the key in this scan's report */
  uint8_t st = state(phys);

  while (draining_ && (st == NONE || st == SWAPPED || st == CONTROL)) {
    if (buffer_len_ < BUFFER_CAPACITY) {
      KeyAddr target = st == SWAPPED ? mirror(phys) : phys;

      appendRelease(phys, clock_, st, target);

      if (st != CONTROL) {
        send(target, IS_PRESSED | WAS_PRESSED);
      }

      setState(phys, NONE);
      return;
    }

    makeRoom();
    st = state(phys);
  }

  releaseNow(phys, key_state);
}

void SwapHandsDygma::releaseNow(KeyAddr phys, uint8_t key_state)
{
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
    if (tt_addr_ != phys || press_ms - tt_release_ms_ >= ::qukeys.holdTimeout(phys)) {
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
    if (pending_ && pending_ctrl_ == idx) {
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

/* Open a decision. Events already in the buffer stay there as its buffer */
void SwapHandsDygma::startPending(uint8_t idx, uint32_t press_ms)
{
  pending_ = true;
  draining_ = false;
  pending_ctrl_ = idx;
  pending_ms_ = press_ms;

  uint16_t hold_timeout = ::qukeys.holdTimeout(controls_[idx].phys);

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
  if (!pending_) {
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

  startDrain();
}

void SwapHandsDygma::resolveTap(uint8_t idx)
{
  if (!pending_) {
    return;
  }

  pending_ = false;

  uint16_t code = controls_[idx].code;

  if (code == ranges::SWAP_HANDS_TAP_TOGGLE) {
    tt_taps_++;
    tt_release_ms_ = clock_;

    if (tt_taps_ >= TAP_TOGGLE_TAPS) {
      swap_ = !swap_;
      tt_taps_ = 0;
    }
  } else {
    uint8_t kc = code - ranges::SWAP_HANDS_TAP_FIRST;

    /* The tap is this scan's event. When one already went out, the tap goes first in the drain */
    if (kc != 0 && sent_this_scan_) {
      tap_deferred_ = true;
      deferred_kc_ = kc;
      deferred_phys_ = controls_[idx].phys;
    } else if (kc != 0) {
      emitTap(controls_[idx].phys, kc);
    }
  }

  startDrain();
}

/* Replay the buffer one event per scan, like the Qukeys queue, so the host gets every key in order in its own report */
void SwapHandsDygma::startDrain()
{
  if (buffer_len_ == 0 && !tap_deferred_ && !sent_this_scan_) {
    return;
  }

  draining_ = true;
  kbdtimer_set_ms(&wake_timer_, 1);

  if (!sent_this_scan_) {
    drainOne();
  }
}

/* Send the deferred tap or the oldest buffered event. A replayed SH_T or SH_TT opens a decision over the rest of the buffer */
void SwapHandsDygma::drainOne()
{
  sent_this_scan_ = true;
  kbdtimer_set_ms(&wake_timer_, 1);

  if (tap_deferred_) {
    tap_deferred_ = false;
    emitTap(deferred_phys_, deferred_kc_);
    return;
  }

  if (buffer_len_ == 0) {
    return;
  }

  entry_t entry = buffer_[0];

  buffer_len_--;
  memmove(&buffer_[0], &buffer_[1], sizeof(entry_t) * buffer_len_);

  uint32_t saved_clock = clock_;

  clock_ = entry.ms;

  if (entry.release) {
    replayRelease(entry);

    /* A release resolved by state leaves the key in NONE. Keep it buffered while a later press of it waits */
    if (state(entry.phys) == NONE && hasQueuedPress(entry.phys, buffer_, buffer_len_)) {
      setState(entry.phys, BUFFERED);
    }
  } else {
    if (state(entry.phys) == BUFFERED) {
      setState(entry.phys, NONE);
    }

    deliverPress(entry.phys, entry.ms);
    queueDeliveredRelease(entry.phys, buffer_, buffer_len_);

    /* Judge the rest of the buffer against the new decision, as if it arrived now */
    if (pending_ && buffer_len_ != 0) {
      entry_t entries[BUFFER_CAPACITY];
      uint8_t count = buffer_len_;

      memcpy(entries, buffer_, sizeof(entry_t) * count);
      buffer_len_ = 0;
      refeed(entries, count);
    }
  }

  clock_ = saved_clock;
}

/* Route buffered events again at their original time. Presses and drained releases queue again, other releases may decide */
void SwapHandsDygma::refeed(entry_t *entries, uint8_t count)
{
  for (uint8_t i = 0; i < count; ++i) {
    const entry_t &entry = entries[i];

    clock_ = entry.ms;

    if (!entry.release) {
      routePress(entry.phys, entry.ms);

      if (state(entry.phys) != BUFFERED) {
        queueDeliveredRelease(entry.phys, entries + i + 1, count - i - 1);
      }
    } else if (entry.snap == BY_STATE) {
      routeRelease(entry.phys, WAS_PRESSED);
    } else if (pending_ || draining_) {
      append(entry);
    } else {
      replayRelease(entry);
    }
  }
}

/* Turn the buffered release of a press just delivered from the buffer into the queued release of a delivered key, so restoreHeld
 * keeps the key in every report until that release goes out */
void SwapHandsDygma::queueDeliveredRelease(KeyAddr phys, entry_t *entries, uint8_t count)
{
  uint8_t st = state(phys);

  if (st != NONE && st != SWAPPED) {
    return;
  }

  for (uint8_t i = 0; i < count; ++i) {
    if (entries[i].release && entries[i].phys == phys && entries[i].snap == BY_STATE) {
      entries[i].snap = st;
      entries[i].target = st == SWAPPED ? mirror(phys) : phys;

      /* Keep a later press of the same key buffered, so its held events wait and its release is bound when it drains */
      setState(phys, hasQueuedPress(phys, entries + i + 1, count - i - 1) ? BUFFERED : NONE);
      return;
    }
  }
}

bool SwapHandsDygma::hasQueuedPress(KeyAddr phys, const entry_t *entries, uint8_t count)
{
  for (uint8_t i = 0; i < count; ++i) {
    if (!entries[i].release && entries[i].phys == phys) {
      return true;
    }
  }

  return false;
}

void SwapHandsDygma::replayRelease(const entry_t &entry)
{
  if (entry.snap == BY_STATE) {
    releaseNow(entry.phys, WAS_PRESSED);
  } else if (entry.snap == CONTROL) {
    controlRelease(entry.phys);
  } else {
    send(entry.target, WAS_PRESSED);
  }
}

/* Keep delivered keys whose release waits in the buffer in the report, as Qukeys does for its queue */
void SwapHandsDygma::restoreHeld()
{
  for (uint8_t i = 0; i < buffer_len_; ++i) {
    if (buffer_[i].release && (buffer_[i].snap == NONE || buffer_[i].snap == SWAPPED)) {
      send(buffer_[i].target, IS_PRESSED | WAS_PRESSED);
    }
  }
}

/* Close the decision and the drain for an upgrade command at once. Buffered swap keys stay inert until released, other keys go
 * out unswapped */
void SwapHandsDygma::cancelPending()
{
  if (pending_) {
    controls_[pending_ctrl_].code = 0;
  }

  pending_ = false;
  draining_ = false;
  tap_deferred_ = false;

  entry_t entries[BUFFER_CAPACITY];
  uint8_t count = buffer_len_;

  memcpy(entries, buffer_, sizeof(entry_t) * count);
  buffer_len_ = 0;

  uint32_t saved_clock = clock_;

  for (uint8_t i = 0; i < count; ++i) {
    const entry_t &entry = entries[i];

    clock_ = entry.ms;

    if (entry.release) {
      replayRelease(entry);
    } else if (isSwapCell(entry.phys)) {
      setState(entry.phys, CONTROL);
    } else {
      setState(entry.phys, NONE);
      deliverPress(entry.phys, entry.ms);
      queueDeliveredRelease(entry.phys, entries + i + 1, count - i - 1);
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
  sent_this_scan_ = true;
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
  /* A press replaces the live keymap entry at its address, so no Superkey SH_MON release can come from there after it */
  if (keyToggledOn(key_state)) {
    bitPut(super_release_, key_addr, false);
  }

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

  if (raw == ranges::SWAP_HANDS_MON && key_addr.isValid() && superHoldEvent(key_addr, key_state)) {
    return EventHandlerResult::OK;
  }

  /* A key mirrored onto an SH_T cell types its tap key. Swap codes act only from the router and a Superkey, so consume every other one */
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

    /* Keep super_release_, so the Superkey's release still reaches Superkeys */
    memset(super_hold_, 0, sizeof(super_hold_));
    super_holds_ = 0;

    if (pending_ || draining_) {
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

/* ------------------------------------------------------------------------- */
/* SH_MON as a Superkey action                                               */
/* ------------------------------------------------------------------------- */

/* Superkeys inject SH_MON at the Superkey's address, and the key's physical release resolves to it through the live keymap. Swap
 * stays on while that address is held. Return true when the event must go on to Superkeys */
bool SwapHandsDygma::superHoldEvent(KeyAddr addr, uint8_t key_state)
{
  if (keyToggledOn(key_state)) {
    if (!(key_state & INJECTED)) {
      return false;
    }

    /* Mark also an action sent after the key was released. Qukeys may still hold that release in its queue */
    bitPut(super_release_, addr, true);

    /* Swap only while the key is held */
    if (Runtime.device().isKeyswitchPressed(addr) && !bitGet(super_hold_, addr)) {
      bitPut(super_hold_, addr, true);
      super_holds_++;
      swap_ = true;
    }

    return false;
  }

  if (!keyToggledOff(key_state)) {
    return false;
  }

  endSuperHold(addr);

  /* Pass the physical release and its Qukeys replay, so Superkeys drops the key from its timeline. Superkeys ignores injected events */
  return !(key_state & INJECTED) && bitGet(super_release_, addr);
}

void SwapHandsDygma::endSuperHold(KeyAddr addr)
{
  if (!bitGet(super_hold_, addr)) {
    return;
  }

  bitPut(super_hold_, addr, false);
  super_holds_--;
  swap_ = false;
}

/* End a Superkey hold whose address is no longer held, so a lost release can't leave swap on */
void SwapHandsDygma::checkSuperHolds()
{
  for (auto addr : KeyAddr::all()) {
    if (bitGet(super_hold_, addr) && !Runtime.device().isKeyswitchPressed(addr)) {
      endSuperHold(addr);
    }
  }
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

/* Judge only presses the router delivered while SH_OS was armed. Keep the mark after a key that does not use SH_OS up, because a
 * Superkey sends its cached modifier at the same address before its action */
void SwapHandsDygma::judgeOneShot(Key key, KeyAddr key_addr, uint8_t key_state)
{
  if (os_ == OS_OFF || !bitGet(os_mark_, key_addr)) {
    return;
  }

  if (keyToggledOff(key_state)) {
    bitPut(os_mark_, key_addr, false);
  } else if (keyToggledOn(key_state) && judgeOneShotKey(key)) {
    bitPut(os_mark_, key_addr, false);
  }
}

/* Keep SH_OS for modifiers, layer keys, OneShot keys, swap keys and unresolved dual-function keys, as OneShot does. Return true
 * when the key uses SH_OS up */
bool SwapHandsDygma::judgeOneShotKey(Key key)
{
  if (os_ == OS_OFF) {
    return false;
  }

  uint16_t raw = key.getRaw();
  uint8_t flags = key.getFlags();
  uint8_t keycode = key.getKeyCode();

  if ((flags & (SYNTHETIC | RESERVED)) == 0 && keycode >= HID_KEYBOARD_FIRST_MODIFIER && keycode <= HID_KEYBOARD_LAST_MODIFIER) {
    return false;
  }

  if (flags == (SYNTHETIC | SWITCH_TO_KEYMAP)) {
    return false;
  }

  if ((raw >= ranges::OS_FIRST && raw <= ranges::OS_LAST) || isSwapCode(raw)) {
    return false;
  }

  if ((raw >= ranges::DU_FIRST && raw <= ranges::DU_LAST) || (raw >= ranges::DYNAMIC_SUPER_FIRST && raw <= ranges::DYNAMIC_SUPER_LAST)) {
    return false;
  }

  if (os_ == OS_PRESSED) {
    os_ = OS_USED;
  } else if (os_ == OS_ACTIVE) {
    endOneShot();
  }

  return true;
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
