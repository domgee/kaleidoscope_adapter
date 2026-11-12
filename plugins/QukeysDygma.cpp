/* -*- mode: c++ -*-
 * Kaleidoscope-Qukeys -- Assign two keycodes to a single key
 * Copyright (C) 2017-2019  Michael Richters
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "QukeysDygma.h"
#include "kbdfal_ll_memory.h"

#include "Kaleidoscope-FocusSerial.h"

#include "kaleidoscope/Runtime.h"
#include <Kaleidoscope-Ranges.h>
#include "kaleidoscope/progmem_helpers.h"
#include "kaleidoscope/layers.h"

#include "SuperkeysHandler.h"

#include "KeyRoleManager.h"
#include "AltRepeatDygma.h"
#include "BaseDygma.h"
#include "CombosDygma.h"
#include "KeyboardioHID.h"
#include "OneShotDygma.h"

#define DEFAULT_HOLD_TIMEOUT_MS              250
#define DEFAULT_OVERLAP_THRESHOLD            80
#define DEFAULT_MINIMUM_HOLD_TIME_MS         50
#define DEFAULT_MINIMUM_PRIOR_INTERVAL_MS    75
#define MAX_QUICK_TAP_TERM_MS                1000

/* Above every prior interval, the global one up to 255 included */
#define PRIOR_INTERVAL_NORMALIZE_MS          255

/* Per-key fields that mean "use the global setting" */
#define DUALFUNC_GLOBAL_U16                  0xFFFF
#define DUALFUNC_GLOBAL_U8                   0xFF
#define DUALFUNC_FIELD_MASK                  0x03
#define DUALFUNC_FIELD_GLOBAL                0x03
#define DUALFUNC_FIELD_INVALID               0x02

/* Positions of the 2-bit fields in DualFunctionKey_t::flags */
#define DUALFUNC_SHIFT_MODE                  0
#define DUALFUNC_SHIFT_CHORDAL               2
#define DUALFUNC_SHIFT_SPECULATIVE           4
#define DUALFUNC_SHIFT_RETRO                 6

/* Bits of DualFunctionHeader_t::options */
#define DUALFUNC_OPT_CHORDAL                 0x01
#define DUALFUNC_OPT_SPECULATIVE             0x02
#define DUALFUNC_OPT_ONE_KEY                 0x04
#define DUALFUNC_OPT_RETRO                   0x08
#define DUALFUNC_OPT_MASK                    0x0F

#define DUALFUNC_MAX_HOLD_TIMEOUT_MS         2000
#define DUALFUNC_MAX_FLOW_TERM_MS            250

/* HID modifier bits the neutralizer covers, as QMK's default MODS_TO_NEUTRALIZE plus the right side. LAlt, LGui, RAlt, RGui */
#define DUALFUNC_NEUTRALIZE_BITS             0xCC

/* The Sonsei matrix has 12 columns, 6 per hand, and a thumb row. Row 4 columns 4..7 (positions 52..55) do not exist */
#define DUALFUNC_THUMB_ROW                   4
#define DUALFUNC_COLS_PER_HAND               6
#define DUALFUNC_FIRST_MISSING_POS           52
#define DUALFUNC_LAST_MISSING_POS            55

namespace kaleidoscope {
namespace plugin {

static_assert(KeyAddr::upper_limit == Qukeys::DUALFUNC_KEYS, "The per-key table needs one entry per matrix position");

Qukeys::DualFunctionKey_t Qukeys::staging_keys_[Qukeys::DUALFUNC_KEYS];

// This is the event handler. It ignores certain events, but mostly just adds
// them to the Qukeys event queue.
EventHandlerResult Qukeys::onKeyswitchEvent(Key& key, KeyAddr k, uint8_t key_state) {
  // If k is not a physical key, ignore it; some other plugin injected it.
  if (! k.isValid() || (key_state & INJECTED) != 0) {
    return EventHandlerResult::OK;
  }

  // If the key was injected (from the queue being flushed), we need to ignore
  // it.
  if (flushing_queue_) {
    return EventHandlerResult::OK;
  }

  /* With Qukeys off, give native DU cells their tap. Leave Superkeys to the Superkeys timeline, since generated combo outputs reach this path too */
  if (! active_) {
    if (decodeNativeDualUse(key, queue_head_.primary_key, queue_head_.alternate_key)) {
      key = queue_head_.primary_key;
    }
    return EventHandlerResult::OK;
  }

  // Deal with keyswitch state changes.
  if (keyToggledOn(key_state) || keyToggledOff(key_state)) {
    // If the user rolled over from a non-modifier key to a qukey, let the
    // release event for that key skip the queue. This prevents unintended
    // repeat characters for the tapped key, which would otherwise have its
    // release event delayed.
    if (keyToggledOff(key_state) && event_queue_.length() == 1 &&
        k != event_queue_.addr(0) && !isModifierKey(key)) {
      noteTapRelease(k, Runtime.millisAtCycleStart());
      return EventHandlerResult::OK;
    }

    /* Clear a stale forced tap left at the index this event takes */
    uint8_t index = event_queue_.length();
    forced_tap_mask_ &= (uint8_t)~(1u << index);

    // If we can't trivially ignore the event, just add it to the queue.
    event_queue_.append(k, key_state);

    if (keyToggledOn(key_state)) {
      grantSpeculation(k, index);
    }

    // In order to prevent overflowing the queue, process it now.
    if (event_queue_.isFull()) {
      /* Keep a flush from overtaking a retro tap. Finish one the host already has, without a report, and drop one it never saw */
      if (retro_stage_ == RETRO_RELEASE) {
        emitRetroRelease();
      } else if (retro_stage_ == RETRO_PRESS) {
        retro_stage_ = RETRO_NONE;
      }

      processQueue();
    }
    // Any event that gets added to the queue gets re-processed later, so we
    // need to abort processing now.
    return EventHandlerResult::EVENT_CONSUMED;
  }

  // The key is being held. We need to determine if we should block it because
  // its key press event is still in the queue, waiting to be
  // flushed. Therefore, we search the event queue for the same key. If the
  // first event we find there is a key press, that means we need to suppress
  // this hold, because it's still waiting on an earlier event.
  for (uint8_t i{0}; i < event_queue_.length(); ++i) {
    if (event_queue_.addr(i) == k) {
      // If the first matching event is a release, we do not suppress it,
      // because its press event has already been flushed.
      if (event_queue_.isRelease(i)) {
        break;
      }
      // Otherwise, the first matching event was a key press, so we need to
      // suppress it for now.
      return EventHandlerResult::EVENT_CONSUMED;
    }
  }

  // Either this key doesn't have an event in the queue at all, or its first
  // event in the queue is a release. We treat the key as a normal held key.
  return EventHandlerResult::OK;
}


// This hook runs once each cycle, and checks to see if the first event in the
// queue is ready to be flushed. It only allows one event to be flushed per
// cycle, because the keyboard HID report can't store all of the information
// necessary to correctly handle all of the rollover corner cases.
EventHandlerResult Qukeys::beforeReportingState() {
  // For keys that have been physically released, but whose release events are
  // still waiting to be flushed from the queue, we need to restore them,
  // because `handleKeyswitchEvent()` didn't get called for those KeyAddrs.
  for (uint8_t i{0}; i < event_queue_.length(); ++i) {
    if (event_queue_.isRelease(i)) {
      KeyAddr k = event_queue_.addr(i);
      // Now for the tricky bit. Before "restoring" this key hold, we need to
      // make sure that its key press event has already been flushed from the
      // queue, so we need to search for a matching key press event preceding
      // this release event. If we find one, we need to ignore it.
      if (isKeyAddrInQueueBeforeIndex(k, i)) {
        continue;
      }
      flushing_queue_ = true;
      handleKeyswitchEvent(Key_NoKey, k, IS_PRESSED | WAS_PRESSED);
      flushing_queue_ = false;
    }
  }

  if (retro_wake_pending_ && kbdtimer_check(&retro_timer_)) {
    retro_wake_pending_ = false;
  }

  /* Give the retro tap's press a report of its own, after the hold's release. The queue waits for the next cycle */
  if (retro_stage_ == RETRO_PRESS) {
    emitRetroPress();
  } else {
    if (retro_stage_ == RETRO_RELEASE) {
      emitRetroRelease();
    }

    // Next, if there hasn't been a keypress in a while, update the prior keypress
    // timestamp to avoid integer overflow issues:
    /* Use the longest possible interval, so a normalized timestamp reads as expired for every key's own interval */
    if (Runtime.hasTimeExpired(prior_keypress_timestamp_,
                               PRIOR_INTERVAL_NORMALIZE_MS)) {
      prior_keypress_timestamp_ =
        Runtime.millisAtCycleStart() - (PRIOR_INTERVAL_NORMALIZE_MS + 1);
    }

    in_report_hook_ = true;

    // If any events get flushed from the queue, stop there; we can only safely
    // send the one report per cycle. Otherwise the first event in the queue is
    // a qukey press, and all that's left to do is to check if it's been held
    // long enough that it has timed out.
    if (!processQueue() && Runtime.hasTimeExpired(event_queue_.timestamp(0), holdTimeout(event_queue_.addr(0)))) {
      settleByTimeout();
    }

    in_report_hook_ = false;
  }

  pressSpeculativeMods();

  return EventHandlerResult::OK;
}

/* Flush the qukey at the queue head as it resolves at its hold timeout */
void Qukeys::settleByTimeout() {
  // If it's a SpaceCadet-type key, it takes on its primary value, otherwise
  // it takes on its secondary value.
  bool spacecadet = isModifierKey(queue_head_.primary_key);

  if (!spacecadet) {
    armRetro(event_queue_.addr(0));
  }

  flushEvent(spacecadet ? queue_head_.primary_key : queue_head_.alternate_key, false, !spacecadet);
}


// -----------------------------------------------------------------------------


// This function contains most of the logic behind Qukeys. It gets called after
// an event gets added to the queue, and again once per cycle. It returns `true`
// if nothing more should be done, either because the queue is empty, or because
// an event has already been flushed. It's not perfect because we might be
// getting more than one event in a given cycle, and because the queue might
// overflow, but those are both rare cases, and should not cause any serious
// problems even when they do come up.
bool Qukeys::processQueue() {
  // If the queue is empty, signal that the beforeReportingState() process
  // should abort before checking for a hold timeout (since there's nothing to
  // do).
  if (event_queue_.isEmpty()) {
    return true;
  }

  // In other cases, we will want the KeyAddr of the first event in the queue.
  KeyAddr queue_head_addr = event_queue_.addr(0);

  // If that first event is a key release, it can be flushed right away.
  if (event_queue_.isRelease(0)) {
    flushEvent(Key_NoKey);
    return true;
  }

  // We now know that the first event is a key press. If it's not a qukey, or if
  // it's only there because the plugin was just turned off, we can flush it
  // immediately.
  if (! isQukey(queue_head_addr) || ! active_) {
    flushEvent(queue_head_.primary_key);
    return true;
  }

  /* Send the tap key at once and keep it held when this press repeats a tap within the quick tap term */
  if (quickTapDue(queue_head_addr)) {
    flushEvent(queue_head_.primary_key, true);
    return true;
  }

  /* Send a qukey that Chordal Hold settled as a tap while an earlier qukey was undecided */
  if ((forced_tap_mask_ & 1) != 0) {
    flushEvent(queue_head_.primary_key, true);
    return true;
  }

  // Now we know that the first event is a key press, and that it's a qukey. In
  // addition, `queue_head_` now contains the primary and secondary Key values
  // for that qukey. As long as the Qukey is at the head of the event queue, we
  // Ask system not to go to sleep until the situation is resolved.

  kbdpwr_sleep_postpone();

  // This variable will be used to record the index in the event queue of the
  // first subsequent key press (after the qukey), if any.
  uint8_t next_keypress_index{0};

  // Next we record if the qukey (at the head of the queue) is a SpaceCadet-type
  // key, so we don't need to do it repeatedly later.
  bool qukey_is_spacecadet = isModifierKey(queue_head_.primary_key);

  // If the qukey press is followed a printable key too closely, it's not
  // eligible to take on its alternate value unless it's a SpaceCadet-type key.
  if (!Runtime.hasTimeExpired(prior_keypress_timestamp_,
                              priorInterval(queue_head_addr)) &&
      !qukey_is_spacecadet) {
    flushEvent(queue_head_.primary_key, true);
    return true;
  }

  /* SpaceCadet qukeys keep the permissive decision and take no Chordal Hold */
  uint16_t hold_timeout = holdTimeout(queue_head_addr);
  uint8_t mode = qukey_is_spacecadet ? DUALFUNC_MODE_PERMISSIVE : modeFor(queue_head_addr);
  bool chordal = !qukey_is_spacecadet && chordalFor(queue_head_addr) && handOf(queue_head_addr) != HAND_THUMB &&
                 !Combos.ownsAddress(queue_head_addr);

  // Now we search the queue for events that will let us decide if the qukey
  // should be flushed (and if so, in which of its two states). We start with
  // the second event in the queue (index 1).
  for (uint8_t i{1}; i < event_queue_.length(); ++i) {
    /* Settle the head by its timeout when a queued event comes at or past its deadline. Only a per-key timeout lets that happen */
    if (has_timeout_override_ && (uint16_t)(event_queue_.timestamp(i) - event_queue_.timestamp(0)) >= hold_timeout) {
      settleByTimeout();
      return true;
    }

    if (event_queue_.isPress(i)) {
      // If some other key was pressed after a SpaceCadet key, that means the
      // SpaceCadet qukey press should be flushed immediately, in its primary
      // (modifier) state. SpaceCadet keys only fall into their alternate state
      // if they are pressed and released with no rollover.
      if (qukey_is_spacecadet) {
        flushEvent(queue_head_.primary_key);
        return true;
      }

      /* Chordal Hold. Let a same-hand qukey stack, and make any other same-hand press a tap of the head and of the qukeys stacked on it */
      if (chordal && sameHandChord(queue_head_addr, i, hold_timeout)) {
        if (isStackableQukeyAt(event_queue_.addr(i))) {
          continue;
        }

        settleSameHandTaps(queue_head_addr, i, hold_timeout);
        flushEvent(queue_head_.primary_key, true);
        return true;
      }

      /* Hold On Other Key Press. Respect the minimum hold time as the permissive hold does */
      if (mode == DUALFUNC_MODE_HOLD_ON_OTHER &&
          Runtime.hasTimeExpired(event_queue_.timestamp(0), p_Qukeys_config->minimum_hold_time)) {
        flushEvent(queue_head_.alternate_key, false, true);
        return true;
      }

      // Otherwise, we've found a subsequent key press, so we record it for the
      // overlap comparison later, unless we've already done so.
      if (next_keypress_index == 0) {
        next_keypress_index = i;
      }
      continue;
    }

    // Now we know the event `i` is a key release. Next, we check to see if it
    // is a release of the qukey.
    if (event_queue_.addr(i) == queue_head_addr) {
      // The qukey (at the head of the queue) was released. If it is a
      // SpaceCadet key, or if no rollover compensation is being used, we can
      // flush it now. Its state depends on whether or not it's a
      // SpaceCadet-type key.
      /* Leave rollover compensation to the permissive mode */
      if (next_keypress_index == 0 || p_Qukeys_config->overlap_threshold == 0 || mode != DUALFUNC_MODE_PERMISSIVE) {
        Key event_key = qukey_is_spacecadet ?
                        queue_head_.alternate_key : queue_head_.primary_key;
        flushEvent(event_key, !qukey_is_spacecadet);
        return true;
      }
      // Now we know the qukey has been released, but we need to check to see if
      // it's release should continue to be delayed during rollover -- if the
      // subsequent key is released soon enough after the qukey is released, it
      // will meet the maximum overlap requirement to make the qukey take on its
      // alternate state.
      uint16_t overlap_start = event_queue_.timestamp(next_keypress_index);
      uint16_t overlap_end = event_queue_.timestamp(i);
      if (releaseDelayed(overlap_start, overlap_end)) {
        continue;
      }
      // The subsequent key was held long enough that the qukey can now be
      // flushed in its primary state. We're treating the rollover as normal
      // typing rollover, not deliberate chording.
      flushEvent(queue_head_.primary_key, true);
      return true;
    }

    /* Chordal Hold. A same-hand key tapped inside the qukey makes it a tap, even after an opposite-hand press */
    if (chordal) {
      for (uint8_t j{1}; j < i; ++j) {
        if (event_queue_.addr(j) == event_queue_.addr(i) && sameHandChord(queue_head_addr, j, hold_timeout)) {
          settleSameHandTaps(queue_head_addr, i, hold_timeout);
          flushEvent(queue_head_.primary_key, true);
          return true;
        }
      }
    }

    /* Timeout only. A nested tap never decides the qukey */
    if (mode == DUALFUNC_MODE_TIMEOUT_ONLY) {
      continue;
    }

    // Event `i` is a key release of some other key than the qukey. Now we check
    // to see if it's also a key that was pressed subsequent to the press of the
    // qukey. We search from the next event after the qukey was pressed, and
    // stop when we get to the release event we're currently looking at.
    for (uint8_t j{1}; j < i; ++j) {
      // If we find an event with a matching KeyAddr, that means there are two
      // events for the same key in the queue after the qukey was pressed. Since
      // the second (or maybe third) event `i` is a key release, even if `j` is
      // not a key press, there must be one in the queue, so it shouldn't be
      // necessary to confirm that `j` is a actually a key press.
      if (event_queue_.addr(j) == event_queue_.addr(i)) {
        // Next, verify that enough time has passed after the qukey was pressed
        // to make it eligible for its alternate value. This helps faster
        // typists avoid unintended modifiers in the output.
        if (Runtime.hasTimeExpired(event_queue_.timestamp(0),
                                   p_Qukeys_config->minimum_hold_time)) {
          flushEvent(queue_head_.alternate_key, false, true);
          return true;
        }
      }
    }
  }

  // Last, since our event queue is finite, we must make sure that there's
  // always room to add another event to the queue by flushing one whenever the
  // queue fills up. We could get multiple events in the same cycle, so this is
  // necessary to avoid reading and writing past the end of the array.
  if (event_queue_.isFull()) {
    flushEvent(queue_head_.primary_key, !qukey_is_spacecadet);
    return true;
  }

  // If we got here, that means we're still waiting for an event (or a timeout)
  // that will determine the state of the qukey. We do know that the event at
  // the head of the queue is a qukey press, and that the `queue_head_.*_key`
  // values are valid. We return false to let the `beforeReportingState()` hook
  // check for hold timeout.
  return false;
}


/* A printable key press sets the prior keypress timestamp */
static bool isPrintable(Key key)
{
  return (key >= Key_A && key <= Key_0) || (key >= Key_Minus && key <= Key_Slash);
}

/* Count a Repeat output by its keycode when Shift is its only flag. Leave shortcuts uncounted, as shortcut cells are */
static bool repeatPrintable(Key key)
{
  return (key.getFlags() & ~SHIFT_HELD) == 0 && isPrintable(Key(key.getKeyCode(), 0));
}

// Flush one event from the head of the queue, with the specified Key value.
void Qukeys::flushEvent(Key event_key, bool quick_tap_arms, bool hold) {
  // First we record the address and state of the event:
  KeyAddr queue_head_addr = event_queue_.addr(0);
  uint8_t keyswitch_state = event_queue_.isRelease(0) ? WAS_PRESSED : IS_PRESSED;

  /* Hand the head's speculative modifier over to its hold. Any other flush cancels every speculation, so it is never granted again */
  if (spec_count_ != 0) {
    if (hold) {
      for (uint8_t i{1}; i < spec_count_; ++i) {
        spec_key_[i - 1] = spec_key_[i];
      }

      --spec_count_;

      if (spec_reported_ != 0) {
        --spec_reported_;
      }
    } else {
      if (in_report_hook_ && spec_reported_ != 0) {
        neutralize(spec_key_, spec_reported_);
      }

      clearSpeculation();
    }
  }

  /* Any other press that reaches the host makes the armed hold a real one */
  if (!event_queue_.isRelease(0) && retro_stage_ == RETRO_ARMED && queue_head_addr != retro_addr_) {
    retro_stage_ = RETRO_NONE;
  }

  bool retro_release = event_queue_.isRelease(0) && retro_stage_ == RETRO_ARMED && queue_head_addr == retro_addr_;

  /* Track the last tap for quick tap. End it at any other press flush, because that key reached the host between the tap and a re-press */
  uint32_t head_ms = widenTimestamp(event_queue_.timestamp(0));

  if (event_queue_.isRelease(0)) {
    noteTapRelease(queue_head_addr, head_ms);
  } else {
    /* Drop the latch at its own press or a later one. Keep it for an earlier press of this address */
    if (queue_head_addr == latch_addr_ && !isBefore(head_ms, latch_ms_)) {
      latch_addr_ = KeyAddr();
    }

    /* Never count a combo action replayed at its anchor as a tap */
    if (quick_tap_arms && quickTapTerm(queue_head_addr) != 0 && !Combos.ownsAddress(queue_head_addr)) {
      tap_addr_ = queue_head_addr;
      tap_released_ = false;
      tap_press_ms_ = head_ms;
    } else {
      tap_addr_ = KeyAddr();
    }
  }

  // If the flushed event is a keypress of a printable symbol, record its
  // timestamp. This lets us suppress some unintended alternate values seen by
  // fast typists by requiring a minimum interval between this keypress and the
  // next qukey press in order for that qukey to become alternate-eligible.
  if (!event_queue_.isRelease(0) && isPrintable(event_key)) {
    prior_keypress_timestamp_ = event_queue_.timestamp(0);
  }

  bool press = !event_queue_.isRelease(0);
  uint16_t press_ts = event_queue_.timestamp(0);

  // Remove the head event from the queue:
  event_queue_.shift();
  forced_tap_mask_ >>= 1;
  // This ensures that the flushed event will be ignored by the event handler hook:
  flushing_queue_ = true;
  handleKeyswitchEvent(event_key, queue_head_addr, keyswitch_state);
  flushing_queue_ = false;

  /* Count a Repeat cell press and a press that resolved to the Repeat key by the key it sent, also when a rule replaced either */
  bool repeat_cell = event_key.getRaw() == RepeatKeyDygma::REPEAT_KEY;

  if (press && (repeat_cell || ::RepeatKey.owns(queue_head_addr)) && repeatPrintable(Layer.lookup(queue_head_addr))) {
    prior_keypress_timestamp_ = press_ts;
  }

  /* The hold is still in this cycle's report, so the neutralizer goes out before the tap in the next one */
  if (retro_release) {
    neutralize(&retro_hold_, 1);
    retro_stage_ = RETRO_PRESS;
  }

  /* Ask for another iteration as soon as possible */
  kbdpwr_sleep_postpone();
}


// Test if the key at address `k` is a qukey. As a side effect, if it is, cache
// that qukey's primary and alternate `Key` values for use later. We do this
// because it's much more efficient than doing that as a separate step.
bool Qukeys::isQukey(KeyAddr k) {
  // First, look up the value from the keymap. We need to do a full lookup, not
  // just looking up the cached value (i.e. `Layer.lookup(k)`), because the
  // cached value will be out of date if a layer change happened since the
  // keyswitch toggled on.
  Key key = Layer.lookupOnActiveLayer(k);

  // Next, we check to see if this is a DualUse-type qukey (defined in the keymap)
  if (isDualUseKey(key)) {
    return true;
  }

  // Last, we check the qukeys array for a match
  uint8_t layer_index = Layer.lookupActiveLayer(k);
  for (uint8_t i{0}; i < qukeys_count_; ++i) {
    Qukey qukey = cloneFromProgmem(qukeys_[i]);
    if (qukey.addr == k) {
      if ((qukey.layer == layer_index) ||
          (qukey.layer == layer_wildcard)) {
        queue_head_.primary_key = key;
        queue_head_.alternate_key = qukey.alternate_key;
        return true;
      }
    }
  }

  // If no matches were found, clear queue_head_ and return false
  queue_head_.primary_key = key;
  queue_head_.alternate_key = Key_Transparent;
  return false;
}

/* Test for a dual-function Superkey or a native DualUse key in the keymap. Like isQukey(), set queue_head_ as a side effect */
bool Qukeys::isDualUseKey(Key key) {
  return decodeDualUse(key, queue_head_.primary_key, queue_head_.alternate_key);
}

/* Decode a dual-function Superkey or a native DualUse key into its tap and hold keys without touching queue_head_. Leave both outputs
 * unchanged for any other key */
bool Qukeys::decodeDualUse(Key key, Key &primary, Key &alternate) {
  return keyRoleManager.dual_function_superkey(key, primary, alternate) || decodeNativeDualUse(key, primary, alternate);
}

/* Decode a native DUM or DUL code into a tap without flags and a plain modifier or layer shift hold. Leave both outputs unchanged for any
 * other key */
bool Qukeys::decodeNativeDualUse(Key key, Key &primary, Key &alternate) {
  // Test for DualUse modifiers:
  if (key >= ranges::DUM_FIRST && key <= ranges::DUM_LAST) {
    uint16_t offset = key.getRaw() - ranges::DUM_FIRST;
    uint8_t modifier_index = (offset >> 8) & 0xFF;  // Upper 8 bits = modifier index
    uint8_t keycode = offset & 0xFF;  // Lower 8 bits = keycode (+ flags offset)
    
    // If keycode >= 128, it means the tap has flags, extract only lower 7 bits
    if (keycode >= 128) {
      keycode = keycode - 128;  // Remove the flags offset
    }
    
    primary = Key(keycode);
    
    // Determine if this is a right modifier by checking the original key's 9th bit
    // Determine the modifier key based on the modifier_index
    Key modifier_base;
    switch (modifier_index) {
        case 0: // Left Control
        case 4: // Right Control
            modifier_base = (modifier_index == 4) ? Key_RightControl : Key_LeftControl;
            break;
        case 1: // Left Shift
        case 5: // Right Shift
            modifier_base = (modifier_index == 5) ? Key_RightShift : Key_LeftShift;
            break;
        case 2: // Left Alt
        case 6: // Right Alt (AltGr)
            modifier_base = (modifier_index == 6) ? Key_RightAlt : Key_LeftAlt;
            break;
        case 3: // Left GUI
        case 7: // Right GUI
            modifier_base = (modifier_index == 7) ? Key_RightGui : Key_LeftGui;
            break;
        default:
            modifier_base = Key_NoKey;
            break;
    }
    
    alternate = modifier_base;
    primary.setFlags(0);

    return true;
  }
  // Test for DualUse layer shifts:
  if (key >= ranges::DUL_FIRST && key <= ranges::DUL_LAST) {
    uint16_t raw_key = key.getRaw();
    //NRF_LOG_DEBUG("isDualUseKey: DUL key found, raw=0x%04X", raw_key);
    
    uint16_t offset = raw_key - ranges::DUL_FIRST;
    int8_t layer = offset >> 8;  // Layer is in upper 8 bits
    
    // If offset >= 128 (in lower 8 bits), it means the tap has flags
    uint16_t keycode_offset = offset & 0xFF;
    bool has_tap_flags = (keycode_offset >= 128);
    if (has_tap_flags) {
      keycode_offset = keycode_offset - 128;  // Remove the flags offset
    }
    
    //NRF_LOG_DEBUG("  offset=0x%04X, layer=%d, keycode=0x%02X, has_flags=%d", 
    //              offset, layer, keycode_offset, has_tap_flags);
    
    primary = Key(keycode_offset);
    primary.setFlags(0);

    alternate = ShiftToLayer(layer);
    //NRF_LOG_DEBUG("  Set alternate key to layer %d", layer);
    return true;
  }
  // It's not a DualUse Key:
  return false;
}


// Return true if the release of the qukey still needs to be delayed due to
// rollover. This is called when a qukey is released before a subsequent key,
// and that key is still being held. It checks to see if the subsequent key has
// been held long enough that the qukey should be flushed in its primary state
// (in which case we return `false`).
bool Qukeys::releaseDelayed(uint16_t overlap_start,
                            uint16_t overlap_end) const {
  // We want to calculate the timeout by dividing the overlap duration by the
  // percentage required to make the qukey take on its alternate state. Since
  // we're doing integer arithmetic, we need to first multiply by 100, then
  // divide by the percentage value (as an integer). We use 32-bit integers
  // here to make sure it doesn't overflow when we multiply by 100.
  uint32_t overlap_duration = overlap_end - overlap_start;
  uint32_t release_timeout = (overlap_duration * 100) / p_Qukeys_config->overlap_threshold;
  return !Runtime.hasTimeExpired(overlap_start, uint16_t(release_timeout));
}


// Helper function for `beforeReportingState()`. In order to restore a key that
// has been physically released, but whose release event has not yet been
// flushed from the queue, we need to be able to search the queue for a given
// release event's corresponding press event, to add only those holds that
// should still be present.
bool Qukeys::isKeyAddrInQueueBeforeIndex(KeyAddr k, uint8_t index) const {
  for (uint8_t i{0}; i < index; ++i) {
    if (event_queue_.addr(i) == k) {
      return true;
    }
  }
  return false;
}

/* Latch a press that directly follows a press of the same address, unless that earlier press went into a combo. Judge the latch when
 * this press reaches the queue head, where the earlier tap is already flushed. Keep the physical time, so a press Combos held back
 * keeps its timing */
void Qukeys::notePhysicalPress(KeyAddr k)
{
  uint32_t now = Runtime.millisAtCycleStart();

  press_gap_ms_ = now - last_press_ms_;
  last_press_ms_ = now;
  disarmRetro();

  if (k == last_press_addr_ && !Combos.ownsAddress(k)) {
    latch_addr_ = k;
    latch_ms_ = Runtime.millisAtCycleStart();
  } else if (k == latch_addr_) {
    /* Drop a latch whose press never reached the queue, such as a combo member */
    latch_addr_ = KeyAddr();
  }

  last_press_addr_ = k;
}

bool Qukeys::quickTapDue(KeyAddr k)
{
  if (k != latch_addr_) {
    return false;
  }

  /* Leave the latch to its own press, since an earlier press of this address reaches the head first when a pending qukey held the queue up */
  if (isBefore(widenTimestamp(event_queue_.timestamp(0)), latch_ms_)) {
    return false;
  }

  latch_addr_ = KeyAddr();

  return quickTapWindow(k) && !isModifierKey(queue_head_.primary_key);
}

/* Return true when a press latched at k repeats its tap within the key's quick tap term */
bool Qukeys::quickTapWindow(KeyAddr k) const
{
  uint16_t term = quickTapTerm(k);

  return term != 0 && k == tap_addr_ && tap_released_ && latch_ms_ - tap_release_ms_ < term;
}

/* Count a tap only when its key came up within the hold timeout */
void Qukeys::noteTapRelease(KeyAddr k, uint32_t release_ms)
{
  if (k != tap_addr_ || tap_released_) {
    return;
  }

  if (release_ms - tap_press_ms_ < holdTimeout(k)) {
    tap_released_ = true;
    tap_release_ms_ = release_ms;
  } else {
    tap_addr_ = KeyAddr();
  }
}

void Qukeys::resetQuickTap()
{
  tap_addr_ = KeyAddr();
  tap_released_ = false;
  last_press_addr_ = KeyAddr();
  latch_addr_ = KeyAddr();
}

uint16_t Qukeys::holdTimeout() const
{
  if (p_Qukeys_config == nullptr || p_Qukeys_config->hold_timeout > 60000) {
    return DEFAULT_HOLD_TIMEOUT_MS;
  }

  return p_Qukeys_config->hold_timeout;
}

uint16_t Qukeys::holdTimeout(KeyAddr k) const
{
  const DualFunctionKey_t *key = keyConfig(k);

  if (key == nullptr || key->hold_timeout == DUALFUNC_GLOBAL_U16) {
    return holdTimeout();
  }

  return key->hold_timeout;
}

/* Cap the term at the key's hold timeout as QMK caps QUICK_TAP_TERM at TAPPING_TERM */
uint16_t Qukeys::quickTapTerm(KeyAddr k) const
{
  if (p_QuickTap_config == nullptr) {
    return 0;
  }

  const DualFunctionKey_t *key = keyConfig(k);
  uint16_t term = (key == nullptr || key->quick_tap_term == DUALFUNC_GLOBAL_U16) ? p_QuickTap_config->quick_tap_term : key->quick_tap_term;
  uint16_t hold_timeout = holdTimeout(k);

  return term < hold_timeout ? term : hold_timeout;
}

/* Return the key's own settings, or nullptr before setup and for an address outside the matrix */
const Qukeys::DualFunctionKey_t *Qukeys::keyConfig(KeyAddr k) const
{
  if (p_DualFunction_config == nullptr || !k.isValid()) {
    return nullptr;
  }

  return &p_DualFunction_config->keys[k.toInt()];
}

uint8_t Qukeys::priorInterval(KeyAddr k) const
{
  const DualFunctionKey_t *key = keyConfig(k);

  if (key == nullptr || key->prior_interval == DUALFUNC_GLOBAL_U8) {
    return p_Qukeys_config->minimum_prior_interval;
  }

  return key->prior_interval;
}

uint8_t Qukeys::modeFor(KeyAddr k) const
{
  const DualFunctionKey_t *key = keyConfig(k);

  if (key != nullptr && (key->flags & DUALFUNC_FIELD_MASK) != DUALFUNC_FIELD_GLOBAL) {
    return key->flags & DUALFUNC_FIELD_MASK;
  }

  return p_DualFunction_config == nullptr ? DUALFUNC_MODE_PERMISSIVE : p_DualFunction_config->header.mode;
}

/* Resolve the on/off field at bit `shift` of the key's flags, or the global option bit when the field says global */
bool Qukeys::optionFor(KeyAddr k, uint8_t shift, uint8_t global_bit) const
{
  const DualFunctionKey_t *key = keyConfig(k);

  if (key != nullptr && ((key->flags >> shift) & DUALFUNC_FIELD_MASK) != DUALFUNC_FIELD_GLOBAL) {
    return ((key->flags >> shift) & DUALFUNC_FIELD_MASK) == 1;
  }

  return p_DualFunction_config != nullptr && (p_DualFunction_config->header.options & global_bit) != 0;
}

bool Qukeys::chordalFor(KeyAddr k) const
{
  return optionFor(k, DUALFUNC_SHIFT_CHORDAL, DUALFUNC_OPT_CHORDAL);
}

bool Qukeys::speculativeFor(KeyAddr k) const
{
  return optionFor(k, DUALFUNC_SHIFT_SPECULATIVE, DUALFUNC_OPT_SPECULATIVE);
}

bool Qukeys::retroFor(KeyAddr k) const
{
  return optionFor(k, DUALFUNC_SHIFT_RETRO, DUALFUNC_OPT_RETRO);
}

/* Return the hand of a matrix position. The thumb row counts as either hand, as QMK's '*' */
uint8_t Qukeys::handOf(KeyAddr k)
{
  if (k.row() == DUALFUNC_THUMB_ROW) {
    return HAND_THUMB;
  }

  return k.col() < DUALFUNC_COLS_PER_HAND ? HAND_LEFT : HAND_RIGHT;
}

/* Return true when the event at index is a press or release on the head's hand, within the head's hold timeout, and not a combo */
bool Qukeys::sameHandChord(KeyAddr head, uint8_t index, uint16_t term) const
{
  KeyAddr k = event_queue_.addr(index);
  uint8_t hand = handOf(k);

  return hand != HAND_THUMB && hand == handOf(head) && !Combos.ownsAddress(k) &&
         (uint16_t)(event_queue_.timestamp(index) - event_queue_.timestamp(0)) < term;
}

/* Mark the same-hand qukeys stacked before index `end` as taps, so they follow the head as taps when they reach the queue head.
 * Skip a qukey whose own timeout ended before the deciding event, so its timeout resolves it */
void Qukeys::settleSameHandTaps(KeyAddr head, uint8_t end, uint16_t term)
{
  for (uint8_t j{1}; j < end; ++j) {
    KeyAddr k = event_queue_.addr(j);

    if (event_queue_.isPress(j) && sameHandChord(head, j, term) && isStackableQukeyAt(k) && chordalFor(k) &&
        (uint16_t)(event_queue_.timestamp(end) - event_queue_.timestamp(j)) < holdTimeout(k)) {
      forced_tap_mask_ |= (uint8_t)(1u << j);
    }
  }
}

/* Return true when the key at k is a qukey whose tap is not a modifier. Leave queue_head_ untouched, since the head's values stay in use */
bool Qukeys::isStackableQukeyAt(KeyAddr k) const
{
  Key key = Layer.lookupOnActiveLayer(k);
  Key primary{Key_Transparent};
  Key alternate{Key_Transparent};

  if (decodeDualUse(key, primary, alternate)) {
    return !isModifierKey(primary);
  }

  uint8_t layer_index = Layer.lookupActiveLayer(k);

  for (uint8_t i{0}; i < qukeys_count_; ++i) {
    Qukey qukey = cloneFromProgmem(qukeys_[i]);

    if (qukey.addr == k && (qukey.layer == layer_index || qukey.layer == layer_wildcard)) {
      return !isModifierKey(key);
    }
  }

  return false;
}

/* Press the hold modifier of a mod-tap qukey at once while it is undecided. Grant it only behind speculated entries, so the
 * speculated entries stay a prefix of the queue */
void Qukeys::grantSpeculation(KeyAddr k, uint8_t index)
{
  if (index != spec_count_ || !active_ || retro_stage_ == RETRO_PRESS || retro_stage_ == RETRO_RELEASE || !speculativeFor(k) ||
      Combos.ownsAddress(k)) {
    return;
  }

  Key key = Layer.lookupOnActiveLayer(k);
  Key primary{Key_Transparent};
  Key alternate{Key_Transparent};

  /* Never speculate on a layer hold or a SpaceCadet key. plainModifierBits rejects layer holds */
  if (!decodeDualUse(key, primary, alternate) || plainModifierBits(alternate) == 0 || isModifierKey(primary)) {
    return;
  }

  /* Skip while a Superkey is pending, and skip a press the prior interval or a quick tap makes a tap */
  if (SuperkeysHandler::hasPendingSuperkey() || !Runtime.hasTimeExpired(prior_keypress_timestamp_, priorInterval(k)) ||
      (k == latch_addr_ && quickTapWindow(k))) {
    return;
  }

  const DualFunctionHeader_t &header = p_DualFunction_config->header;
  auto &keyboard = Runtime.hid().keyboard();

  if ((header.options & DUALFUNC_OPT_ONE_KEY) != 0 && (spec_count_ != 0 || keyboard.wasAnyModifierKeyActive())) {
    return;
  }

  /* Skip a press Combos held back behind a later one, and a press inside the flow term after the one before */
  if (k != last_press_addr_ || (header.speculative_flow_term != 0 && press_gap_ms_ < header.speculative_flow_term)) {
    return;
  }

  if (keyboard.wasModifierKeyActive(alternate)) {
    return;
  }

  spec_key_[spec_count_++] = alternate;
}

void Qukeys::clearSpeculation()
{
  spec_count_ = 0;
  spec_reported_ = 0;
}

/* Press at the end of the report hook only, so the live keymap cache and other plugins never see a speculative modifier */
void Qukeys::pressSpeculativeMods()
{
  for (uint8_t i{0}; i < spec_count_; ++i) {
    Runtime.hid().keyboard().pressKey(spec_key_[i], false);
  }

  /* BaseDygma empties the report while key reports are off */
  spec_reported_ = ::BaseDygma.isKeyReportEnabled() ? spec_count_ : 0;
}

/* Arm a retro tap for a qukey that resolved to hold at its timeout with no other key pressed since */
void Qukeys::armRetro(KeyAddr k)
{
  if (!retroFor(k) || Combos.ownsAddress(k) || k != last_press_addr_) {
    return;
  }

  for (uint8_t i{1}; i < event_queue_.length(); ++i) {
    if (event_queue_.isPress(i)) {
      return;
    }
  }

  retro_addr_ = k;
  retro_tap_ = queue_head_.primary_key;
  retro_hold_ = queue_head_.alternate_key;
  retro_stage_ = RETRO_ARMED;
}

/* Leave a retro tap already on its way, so its release always follows its press */
void Qukeys::disarmRetro()
{
  if (retro_stage_ == RETRO_ARMED) {
    retro_stage_ = RETRO_NONE;
  }
}

void Qukeys::emitRetroPress()
{
  flushing_queue_ = true;
  handleKeyswitchEvent(retro_tap_, retro_addr_, IS_PRESSED);
  flushing_queue_ = false;

  /* Release the key the press left in the live cache, which keeps a rewrite such as AltRepeat's */
  retro_release_key_ = Layer.lookup(retro_addr_);

  if (isPrintable(retro_tap_) || (::RepeatKey.owns(retro_addr_) && repeatPrintable(retro_release_key_))) {
    prior_keypress_timestamp_ = Runtime.millisAtCycleStart();
  }

  /* A retro tap never arms quick tap */
  tap_addr_ = KeyAddr();

  retro_stage_ = RETRO_RELEASE;

  /* Wake for the release, since nothing else may postpone sleep */
  kbdtimer_set_ms(&retro_timer_, 1);
  retro_wake_pending_ = true;
}

void Qukeys::emitRetroRelease()
{
  flushing_queue_ = true;
  handleKeyswitchEvent(retro_release_key_, retro_addr_, WAS_PRESSED);
  flushing_queue_ = false;

  retro_stage_ = RETRO_NONE;
}

/* Send the neutralizer key while Alt or GUI from these keys is still down, so their release alone opens no menu. Act only on bits the
 * host has, after any KeyOverrides filter, and add only flag-free modifiers so a filtered bit never reaches the host */
void Qukeys::neutralize(const Key *keys, uint8_t n)
{
  if (!in_report_hook_ || p_DualFunction_config == nullptr || p_DualFunction_config->header.neutralizer == 0 ||
      !::BaseDygma.isKeyReportEnabled()) {
    return;
  }

  auto &keyboard = Runtime.hid().keyboard();
  uint8_t bits = 0;

  for (uint8_t i{0}; i < n; ++i) {
    bits |= plainModifierBits(keys[i]);
  }

  uint8_t seen = 0;

  for (uint8_t b{0}; b < 8; ++b) {
    if ((bits & (1u << b)) != 0 && keyboard.wasModifierKeyActive(Key(HID_KEYBOARD_FIRST_MODIFIER + b, KEY_FLAGS))) {
      seen |= (uint8_t)(1u << b);
    }
  }

  if ((seen & DUALFUNC_NEUTRALIZE_BITS) == 0) {
    return;
  }

  uint8_t code = p_DualFunction_config->header.neutralizer;
  Key dummy(code, KEY_FLAGS);

  /* Leave a neutralizer key that is held for real alone */
  if (code >= HID_KEYBOARD_FIRST_MODIFIER) {
    if (keyboard.isModifierKeyActive(dummy) || keyboard.wasModifierKeyActive(dummy)) {
      return;
    }
  } else if (::Keyboard.isKeyPressed(code) || ::Keyboard.wasKeyPressed(code)) {
    return;
  }

  uint8_t before = 0;

  for (uint8_t b{0}; b < 8; ++b) {
    if (keyboard.isModifierKeyActive(Key(HID_KEYBOARD_FIRST_MODIFIER + b, KEY_FLAGS))) {
      before |= (uint8_t)(1u << b);
    }
  }

  for (uint8_t b{0}; b < 8; ++b) {
    if ((seen & (1u << b)) != 0) {
      keyboard.pressRawKey(Key(HID_KEYBOARD_FIRST_MODIFIER + b, KEY_FLAGS));
    }
  }

  pressOneShotModifiers();

  keyboard.pressRawKey(dummy);
  keyboard.sendReport();
  keyboard.releaseRawKey(dummy);
  keyboard.sendReport();

  /* Release only what this call added. The scan and OneShot press their own keys again */
  for (uint8_t b{0}; b < 8; ++b) {
    Key modifier(HID_KEYBOARD_FIRST_MODIFIER + b, KEY_FLAGS);

    if ((before & (1u << b)) == 0 && keyboard.isModifierKeyActive(modifier)) {
      keyboard.releaseRawKey(modifier);
    }
  }
}

/* Return the HID modifier bits a plain modifier key holds, with its flags, as KeyOverrides counts them. 0 for any other key */
uint8_t Qukeys::plainModifierBits(Key key)
{
  if ((key.getFlags() & (SYNTHETIC | RESERVED)) != 0 || key.getKeyCode() < HID_KEYBOARD_FIRST_MODIFIER ||
      key.getKeyCode() > HID_KEYBOARD_LAST_MODIFIER) {
    return 0;
  }

  return (uint8_t)((1u << (key.getKeyCode() - HID_KEYBOARD_FIRST_MODIFIER)) | flagsToHid(key.getFlags()));
}

/* Key flags as HID bits, as Kaleidoscope's pressModifiers sends them */
uint8_t Qukeys::flagsToHid(uint8_t flags)
{
  uint8_t hid = 0;

  if (flags & CTRL_HELD) {
    hid |= 0x01;
  }

  if (flags & SHIFT_HELD) {
    hid |= 0x02;
  }

  if (flags & LALT_HELD) {
    hid |= 0x04;
  }

  if (flags & GUI_HELD) {
    hid |= 0x08;
  }

  if (flags & RALT_HELD) {
    hid |= 0x40;
  }

  return hid;
}

/* OneShot presses its modifiers in its own report hook, after this one, so a mid-cycle report needs them here */
void Qukeys::pressOneShotModifiers()
{
  for (uint8_t keycode = HID_KEYBOARD_FIRST_MODIFIER; keycode <= HID_KEYBOARD_LAST_MODIFIER; keycode++) {
    Key key(keycode, KEY_FLAGS);

    if (::OneShot.isModifierActive(key)) {
      Runtime.hid().keyboard().pressKey(key, false);
    }
  }
}

/* Widen a queue timestamp to 32 bits. Rely on a queued event being at most a hold timeout old, well under 65536 ms */
uint32_t Qukeys::widenTimestamp(uint16_t timestamp)
{
  uint32_t now = Runtime.millisAtCycleStart();

  return now - (uint16_t)((uint16_t)now - timestamp);
}

/* Order two times wrap-safe, so the comparison holds across the 32-bit millis wrap */
bool Qukeys::isBefore(uint32_t a, uint32_t b)
{
  return (int32_t)(a - b) < 0;
}


// -----------------------------------------------------------------------------

// This function could get lifted into Kaleidoscope proper, since it might be
// more generally useful. It's here to provide the test for a SpaceCadet-type
// qukey, which is any Qukey that has a modifier (including layer shifts) as its
// primary value.
bool isModifierKey(Key key) {
  // If it's a plain keyboard key, return true if its base keycode is a
  // modifier, otherwise return false:
  if ((key.getFlags() & (SYNTHETIC | RESERVED)) == 0) {
    return (key.getKeyCode() >= HID_KEYBOARD_FIRST_MODIFIER &&
            key.getKeyCode() <= HID_KEYBOARD_LAST_MODIFIER);
  }
  // If it's a layer shift key, return true:
  if (key.getFlags() == (SYNTHETIC | SWITCH_TO_KEYMAP) &&
      key.getKeyCode() >= LAYER_SHIFT_OFFSET) {
    return true;
  }
  // In all other cases, return false:
  return false;
}

/* Skip spaces and return true at the end of the line. Leave the newline for FocusSerial */
static bool readLineEnd()
{
  int c = AltRepeatDygma::peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = AltRepeatDygma::peekWait();
  }

  return c == '\n';
}

/* Read the rest of a rejected line here, because FocusSerial::drain does not wait for bytes still in transit */
static void rejectLine()
{
  for (int c = AltRepeatDygma::peekWait(); c >= 0 && c != '\n'; c = AltRepeatDygma::peekWait()) {
    Runtime.serialPort().read();
  }

  ::Focus.send("error");
}

/* Read one decimal of 0 to MAX_QUICK_TAP_TERM_MS followed by the end of the line. Reject anything else */
static bool readQuickTapTerm(uint16_t &term)
{
  if (!AltRepeatDygma::readValue(term) || term > MAX_QUICK_TAP_TERM_MS) {
    return false;
  }

  return readLineEnd();
}

static bool neutralizerAllowed(uint8_t keycode)
{
  return keycode == 0 || (keycode >= HID_KEYBOARD_A_AND_A && keycode <= HID_KEYPAD_HEXADECIMAL) ||
         (keycode >= HID_KEYBOARD_FIRST_MODIFIER && keycode <= HID_KEYBOARD_LAST_MODIFIER);
}

static bool positionExists(uint8_t pos)
{
  return pos < Qukeys::DUALFUNC_KEYS && (pos < DUALFUNC_FIRST_MISSING_POS || pos > DUALFUNC_LAST_MISSING_POS);
}

static bool isGlobalKey(const Qukeys::DualFunctionKey_t &key)
{
  return key.hold_timeout == DUALFUNC_GLOBAL_U16 && key.quick_tap_term == DUALFUNC_GLOBAL_U16 && key.prior_interval == DUALFUNC_GLOBAL_U8 &&
         key.flags == DUALFUNC_GLOBAL_U8;
}

EventHandlerResult Qukeys::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, PSTR("qukeys.holdTimeout\nqukeys.overlapThreshold\nqukeys.minimumHoldTime\nqukeys.minimumPriorInterval\n"
                                       "qukeys.quickTapTerm\nqukeys.dualFunction\nqukeys.perKey"))) {
    return EventHandlerResult::OK;
  }

  if (strncmp_P(command, PSTR("qukeys."), 7) != 0)
    return EventHandlerResult::OK;

  /* Drop a speculative modifier and an armed retro tap at any write, since the settings they came from may change */
  if (!::Focus.isEOL())
  {
    clearSpeculation();
    disarmRetro();
  }

  if (strcmp_P(command + 7, PSTR("holdTimeout")) == 0)
  {
    if (::Focus.isEOL())
    {
      ::Focus.send(p_Qukeys_config->hold_timeout);
    }
    else
    {
      uint16_t newHold = 0;
      uint8_t a{0};
      uint8_t b{0};
      ::Focus.read(a);
      while (!::Focus.isEOL())
      {
        ::Focus.read(b);
      }
      newHold = ((b << 8) | a);

      mem_hold_timeout_save( newHold );
    }
  }

  if (strcmp_P(command + 7, PSTR("overlapThreshold")) == 0)
  {
    if (::Focus.isEOL())
    {
      ::Focus.send(p_Qukeys_config->overlap_threshold);
    }
    else
    {
      uint8_t percent;
      ::Focus.read(percent);

      mem_overlap_threshold_save( percent );
    }
  }
  
  if (strcmp_P(command + 7, PSTR("minimumHoldTime")) == 0)
  {
    if (::Focus.isEOL())
    {
      ::Focus.send(p_Qukeys_config->minimum_hold_time);
    }
    else
    {
      uint8_t minimum;
      ::Focus.read(minimum);

      mem_minimum_hold_time_save( minimum );
    }
  }

  if (strcmp_P(command + 7, PSTR("minimumPriorInterval")) == 0)
  {
    if (::Focus.isEOL())
    {
      ::Focus.send(p_Qukeys_config->minimum_prior_interval);
    }
    else
    {
      uint8_t prior;
      ::Focus.read(prior);

      mem_minimum_prior_interval_save( prior );
    }
  }

  if (strcmp_P(command + 7, PSTR("quickTapTerm")) == 0)
  {
    if (::Focus.isEOL())
    {
      ::Focus.send(p_QuickTap_config->quick_tap_term);
    }
    else
    {
      uint16_t term = 0;

      if (readQuickTapTerm(term))
      {
        mem_quick_tap_term_save( term );
        resetQuickTap();
      }
      else
      {
        rejectLine();
      }
    }
  }

  if (strcmp_P(command + 7, PSTR("dualFunction")) == 0)
  {
    DualFunctionHeader_t header;

    if (::Focus.isEOL())
    {
      sendDualFunction();
    }
    else if (readDualFunction(header))
    {
      mem_dualfunc_header_save( header );
    }
    else
    {
      rejectLine();
    }
  }

  if (strcmp_P(command + 7, PSTR("perKey")) == 0)
  {
    if (::Focus.isEOL())
    {
      sendPerKey();
    }
    else if (readPerKey())
    {
      mem_dualfunc_keys_save();
      updateTimeoutOverride();
      resetQuickTap();
    }
    else
    {
      rejectLine();
    }
  }

  return EventHandlerResult::EVENT_CONSUMED;
}

/* Send <mode> <chordal> <speculative> <oneKey> <flowTerm> <retro> <neutralizer> */
void Qukeys::sendDualFunction() const
{
  const DualFunctionHeader_t &header = p_DualFunction_config->header;

  ::Focus.send(header.mode, (header.options & DUALFUNC_OPT_CHORDAL) != 0 ? 1 : 0, (header.options & DUALFUNC_OPT_SPECULATIVE) != 0 ? 1 : 0,
               (header.options & DUALFUNC_OPT_ONE_KEY) != 0 ? 1 : 0, header.speculative_flow_term, (header.options & DUALFUNC_OPT_RETRO) != 0 ? 1 : 0,
               header.neutralizer);
}

/* Parse exactly seven decimals in the order sendDualFunction sends them. Return false on a missing, malformed or out of range value */
bool Qukeys::readDualFunction(DualFunctionHeader_t &header)
{
  uint16_t v[7];

  for (uint8_t i = 0; i < 7; ++i) {
    if (!AltRepeatDygma::readValue(v[i])) {
      return false;
    }
  }

  if (!readLineEnd() || v[0] > DUALFUNC_MODE_TIMEOUT_ONLY || v[1] > 1 || v[2] > 1 || v[3] > 1 || v[4] > DUALFUNC_MAX_FLOW_TERM_MS ||
      v[5] > 1 || v[6] > 0xFF) {
    return false;
  }

  memset(&header, 0, sizeof(header));
  header.layout_version = DUALFUNC_LAYOUT_VERSION;
  header.mode = (uint8_t)v[0];
  header.options = (v[1] != 0 ? DUALFUNC_OPT_CHORDAL : 0) | (v[2] != 0 ? DUALFUNC_OPT_SPECULATIVE : 0) | (v[3] != 0 ? DUALFUNC_OPT_ONE_KEY : 0) |
                   (v[5] != 0 ? DUALFUNC_OPT_RETRO : 0);
  header.speculative_flow_term = (uint8_t)v[4];
  header.neutralizer = (uint8_t)v[6];

  return headerAllowed(header);
}

/* Send <count> and then <pos> <holdTimeout> <quickTapTerm> <priorInterval> <mode> <chordal> <speculative> <retro> for each key with an override */
void Qukeys::sendPerKey() const
{
  uint8_t count = 0;

  for (uint8_t pos = 0; pos < DUALFUNC_KEYS; ++pos) {
    if (!isGlobalKey(p_DualFunction_config->keys[pos])) {
      ++count;
    }
  }

  ::Focus.send(count);

  for (uint8_t pos = 0; pos < DUALFUNC_KEYS; ++pos) {
    const DualFunctionKey_t &key = p_DualFunction_config->keys[pos];

    if (isGlobalKey(key)) {
      continue;
    }

    uint8_t flags = key.flags;
    uint8_t mode = (flags >> DUALFUNC_SHIFT_MODE) & DUALFUNC_FIELD_MASK;
    uint8_t chordal = (flags >> DUALFUNC_SHIFT_CHORDAL) & DUALFUNC_FIELD_MASK;
    uint8_t speculative = (flags >> DUALFUNC_SHIFT_SPECULATIVE) & DUALFUNC_FIELD_MASK;
    uint8_t retro = (flags >> DUALFUNC_SHIFT_RETRO) & DUALFUNC_FIELD_MASK;

    ::Focus.send(pos, key.hold_timeout, key.quick_tap_term, key.prior_interval, mode, chordal, speculative, retro);
  }
}

/* Parse a whole table into staging_keys_ in the format sendPerKey sends. Return false on the first missing, malformed, out of range or
 * duplicate value, or an entry a load would reject */
bool Qukeys::readPerKey()
{
  memset(staging_keys_, DUALFUNC_GLOBAL_U8, sizeof(staging_keys_));

  uint64_t seen = 0;
  uint16_t count = 0;

  if (!AltRepeatDygma::readValue(count) || count > DUALFUNC_KEYS - (DUALFUNC_LAST_MISSING_POS - DUALFUNC_FIRST_MISSING_POS + 1)) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t v[8];

    for (uint8_t f = 0; f < 8; ++f) {
      if (!AltRepeatDygma::readValue(v[f])) {
        return false;
      }
    }

    if (v[0] > 0xFF || !positionExists((uint8_t)v[0]) || (seen & (1ULL << v[0])) != 0) {
      return false;
    }

    if (v[3] > 0xFF || v[4] > DUALFUNC_FIELD_MASK || v[5] > DUALFUNC_FIELD_MASK || v[6] > DUALFUNC_FIELD_MASK || v[7] > DUALFUNC_FIELD_MASK) {
      return false;
    }

    DualFunctionKey_t key;
    key.hold_timeout = v[1];
    key.quick_tap_term = v[2];
    key.prior_interval = (uint8_t)v[3];
    key.flags = (uint8_t)((v[4] << DUALFUNC_SHIFT_MODE) | (v[5] << DUALFUNC_SHIFT_CHORDAL) | (v[6] << DUALFUNC_SHIFT_SPECULATIVE) |
                          (v[7] << DUALFUNC_SHIFT_RETRO));

    if (!keyAllowed((uint8_t)v[0], key)) {
      return false;
    }

    staging_keys_[v[0]] = key;
    seen |= 1ULL << v[0];
  }

  return readLineEnd();
}

bool Qukeys::headerAllowed(const DualFunctionHeader_t &header)
{
  return header.mode <= DUALFUNC_MODE_TIMEOUT_ONLY && (header.options & (uint8_t)~DUALFUNC_OPT_MASK) == 0 &&
         header.speculative_flow_term <= DUALFUNC_MAX_FLOW_TERM_MS && neutralizerAllowed(header.neutralizer) && header.reserved[0] == 0 &&
         header.reserved[1] == 0 && header.reserved[2] == 0;
}

/* Check one table entry. Load and write share it, so storage only holds what a write accepts */
bool Qukeys::keyAllowed(uint8_t pos, const DualFunctionKey_t &key)
{
  if (!positionExists(pos)) {
    return isGlobalKey(key);
  }

  if (key.hold_timeout != DUALFUNC_GLOBAL_U16 && (key.hold_timeout == 0 || key.hold_timeout > DUALFUNC_MAX_HOLD_TIMEOUT_MS)) {
    return false;
  }

  if (key.quick_tap_term != DUALFUNC_GLOBAL_U16 && key.quick_tap_term > MAX_QUICK_TAP_TERM_MS) {
    return false;
  }

  /* The mode field takes every 2-bit value, the on/off fields never take 2 */
  return ((key.flags >> DUALFUNC_SHIFT_CHORDAL) & DUALFUNC_FIELD_MASK) != DUALFUNC_FIELD_INVALID &&
         ((key.flags >> DUALFUNC_SHIFT_SPECULATIVE) & DUALFUNC_FIELD_MASK) != DUALFUNC_FIELD_INVALID &&
         ((key.flags >> DUALFUNC_SHIFT_RETRO) & DUALFUNC_FIELD_MASK) != DUALFUNC_FIELD_INVALID;
}

/* Start with every option off and every key global on erased flash or another layout. Reset a header or entry a write would reject */
void Qukeys::loadDualFunction()
{
  DualFunctionHeader_t header;

  memset(&header, 0, sizeof(header));
  header.layout_version = DUALFUNC_LAYOUT_VERSION;

  if (p_DualFunction_config->header.layout_version != DUALFUNC_LAYOUT_VERSION) {
    mem_dualfunc_header_save( header );
    memset(staging_keys_, DUALFUNC_GLOBAL_U8, sizeof(staging_keys_));
    mem_dualfunc_keys_save();
  } else {
    if (!headerAllowed(p_DualFunction_config->header)) {
      mem_dualfunc_header_save( header );
    }

    bool changed = false;

    memcpy(staging_keys_, p_DualFunction_config->keys, sizeof(staging_keys_));

    for (uint8_t pos = 0; pos < DUALFUNC_KEYS; ++pos) {
      if (!keyAllowed(pos, staging_keys_[pos])) {
        memset(&staging_keys_[pos], DUALFUNC_GLOBAL_U8, sizeof(staging_keys_[pos]));
        changed = true;
      }
    }

    if (changed) {
      mem_dualfunc_keys_save();
    }
  }

  updateTimeoutOverride();
}

void Qukeys::updateTimeoutOverride()
{
  has_timeout_override_ = false;

  for (uint8_t pos = 0; pos < DUALFUNC_KEYS; ++pos) {
    if (p_DualFunction_config->keys[pos].hold_timeout != DUALFUNC_GLOBAL_U16) {
      has_timeout_override_ = true;
    }
  }
}

EventHandlerResult Qukeys::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_QUKEYS, (const void **)&p_Qukeys_config );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  if( p_Qukeys_config->hold_timeout > 60000 )
  {
    mem_hold_timeout_save( DEFAULT_HOLD_TIMEOUT_MS );
  }

  if( p_Qukeys_config->overlap_threshold > 100 )
  {
    mem_overlap_threshold_save( DEFAULT_OVERLAP_THRESHOLD );
  }

  if( p_Qukeys_config->minimum_hold_time > 254 )
  {
    mem_minimum_hold_time_save( DEFAULT_MINIMUM_HOLD_TIME_MS );
  }

  if( p_Qukeys_config->minimum_prior_interval > 254 )
  {
    mem_minimum_prior_interval_save( DEFAULT_MINIMUM_PRIOR_INTERVAL_MS );
  }

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_QUICKTAP, (const void **)&p_QuickTap_config );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Start with quick tap off when the stored term is out of range, such as 0xFFFF from erased flash */
  if( p_QuickTap_config->quick_tap_term > MAX_QUICK_TAP_TERM_MS )
  {
    mem_quick_tap_term_save( 0 );
  }

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_DUALFUNC, (const void **)&p_DualFunction_config );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  loadDualFunction();

  return EventHandlerResult::OK;

  UNUSED( result );
}

void Qukeys::mem_hold_timeout_save( uint16_t hold_timeout )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_Qukeys_config->hold_timeout, &hold_timeout, sizeof(p_Qukeys_config->hold_timeout) );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_save failed" );

    UNUSED( result );
}

void Qukeys::mem_overlap_threshold_save( uint8_t overlap_threshold )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_Qukeys_config->overlap_threshold, &overlap_threshold, sizeof(p_Qukeys_config->overlap_threshold) );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_save failed" );

    UNUSED( result );
}

void Qukeys::mem_minimum_hold_time_save( uint8_t minimum_hold_time )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_Qukeys_config->minimum_hold_time, &minimum_hold_time, sizeof(p_Qukeys_config->minimum_hold_time) );
    ASSERT_DYGMA( result == RESULT_OK, "minimum_hold_time failed" );

    UNUSED( result );
}

void Qukeys::mem_minimum_prior_interval_save( uint8_t minimum_prior_interval )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_Qukeys_config->minimum_prior_interval, &minimum_prior_interval, sizeof(p_Qukeys_config->minimum_prior_interval) );
    ASSERT_DYGMA( result == RESULT_OK, "minimum_prior_interval failed" );

    UNUSED( result );
}

void Qukeys::mem_quick_tap_term_save( uint16_t quick_tap_term )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_QuickTap_config->quick_tap_term, &quick_tap_term, sizeof(p_QuickTap_config->quick_tap_term) );
    ASSERT_DYGMA( result == RESULT_OK, "quick_tap_term failed" );

    UNUSED( result );
}

void Qukeys::mem_dualfunc_header_save( const DualFunctionHeader_t &header )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_DualFunction_config->header, &header, sizeof(DualFunctionHeader_t) );
    ASSERT_DYGMA( result == RESULT_OK, "dualfunc header failed" );

    UNUSED( result );
}

/* Save the whole table from staging_keys_ */
void Qukeys::mem_dualfunc_keys_save()
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( p_DualFunction_config->keys, staging_keys_, sizeof(staging_keys_) );
    ASSERT_DYGMA( result == RESULT_OK, "dualfunc keys failed" );

    UNUSED( result );
}

} // namespace plugin {
} // namespace kaleidoscope {

kaleidoscope::plugin::Qukeys qukeys;
