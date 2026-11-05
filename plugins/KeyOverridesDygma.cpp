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

#include "KeyOverridesDygma.h"
#include "kbdfal_ll_memory.h"

#include "AltRepeatDygma.h"
#include "BaseDygma.h"
#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"
#include "kaleidoscope/layers.h"
#include "KeyboardioHID.h"
#include "LayerHoldModsDygma.h"
#include "OneShotDygma.h"
#include "QukeysDygma.h"

namespace kaleidoscope {
namespace plugin {

KeyOverridesDygma::keyoverrides_config_t KeyOverridesDygma::staging_;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* HID modifier byte bits. Right side modifiers sit four bits higher */
static constexpr uint8_t HID_CTRL  = 0x01;
static constexpr uint8_t HID_SHIFT = 0x02;
static constexpr uint8_t HID_ALT   = 0x04;
static constexpr uint8_t HID_GUI   = 0x08;
static constexpr uint8_t HID_RALT  = 0x40;

/* Both sides of each modifier in Key flags, as HID bits */
static uint8_t bothSides(uint8_t mods)
{
  uint8_t hid = 0;

  if (mods & CTRL_HELD) {
    hid |= 0x11;
  }

  if (mods & SHIFT_HELD) {
    hid |= 0x22;
  }

  if (mods & (LALT_HELD | RALT_HELD)) {
    hid |= 0x44;
  }

  if (mods & GUI_HELD) {
    hid |= 0x88;
  }

  return hid;
}

/* Key flags as HID bits, as Kaleidoscope's pressModifiers sends them */
static uint8_t flagsToHid(uint8_t flags)
{
  uint8_t hid = 0;

  if (flags & CTRL_HELD) {
    hid |= HID_CTRL;
  }

  if (flags & SHIFT_HELD) {
    hid |= HID_SHIFT;
  }

  if (flags & LALT_HELD) {
    hid |= HID_ALT;
  }

  if (flags & GUI_HELD) {
    hid |= HID_GUI;
  }

  if (flags & RALT_HELD) {
    hid |= HID_RALT;
  }

  return hid;
}

/* HID bits as side-folded Key flags. AltGr counts as Alt */
static uint8_t foldHid(uint8_t hid)
{
  uint8_t sides = (uint8_t)((hid | (hid >> 4)) & 0x0F);
  uint8_t mods = 0;

  if (sides & HID_CTRL) {
    mods |= CTRL_HELD;
  }

  if (sides & HID_SHIFT) {
    mods |= SHIFT_HELD;
  }

  if (sides & HID_ALT) {
    mods |= LALT_HELD;
  }

  if (sides & HID_GUI) {
    mods |= GUI_HELD;
  }

  return mods;
}

/* Fold AltGr into Alt and keep Ctrl, Alt, Shift and Gui */
static uint8_t foldFlags(uint8_t flags)
{
  if (flags & RALT_HELD) {
    flags |= LALT_HELD;
  }

  return flags & KeyOverridesDygma::MODS_MASK;
}

static bool isModifier(Key key)
{
  if (key.getFlags() & (SYNTHETIC | RESERVED)) {
    return false;
  }

  return key.getKeyCode() >= HID_KEYBOARD_FIRST_MODIFIER && key.getKeyCode() <= HID_KEYBOARD_LAST_MODIFIER;
}

/* The HID bits a modifier key holds. Flags on a modifier key are held with it, as Kaleidoscope's pressKey does */
static uint8_t modifierBits(Key key)
{
  return (uint8_t)((1 << (key.getKeyCode() - HID_KEYBOARD_FIRST_MODIFIER)) | flagsToHid(key.getFlags()));
}

static bool getBit(const uint8_t *bits, uint8_t offset)
{
  return (bits[offset >> 3] & (1 << (offset & 7))) != 0;
}

static void setBit(uint8_t *bits, uint8_t offset, bool value)
{
  if (value) {
    bits[offset >> 3] |= (uint8_t)(1 << (offset & 7));
  } else {
    bits[offset >> 3] &= (uint8_t) ~(1 << (offset & 7));
  }
}

void KeyOverridesDygma::setMuted(uint8_t offset, bool value)
{
  if (offset >= MAX_TRACKED_KEYS) {
    return;
  }

  setBit(muted_, offset, value);

  if (value) {
    muted_any_ = true;
  }
}

/* ------------------------------------------------------------------------- */
/* Setup, validity, held                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult KeyOverridesDygma::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_KEYOVERRIDES, (const void **)&p_config_ );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Write an empty table over erased flash or another layout, which happens once after a flash */
  if (!configValid()) {
    memset(&staging_, 0, sizeof(staging_));
    staging_.layout_version = LAYOUT_VERSION;
    cfgmem_config_save(&staging_);
  }

  count_ = configValid() ? p_config_->count : 0;

  UNUSED( result );

  return EventHandlerResult::OK;
}

bool KeyOverridesDygma::configValid() const
{
  return p_config_ != nullptr && p_config_->layout_version == LAYOUT_VERSION && p_config_->count <= MAX_OVERRIDES;
}

/* Count a key whose release still waits in the Qukeys queue as held */
bool KeyOverridesDygma::isHeld(uint8_t offset) const
{
  KeyAddr key_addr(offset);

  return Runtime.device().isKeyswitchPressed(key_addr) || qukeys.hasQueuedReleaseFirst(key_addr);
}

/* ------------------------------------------------------------------------- */
/* Modifier state                                                            */
/* ------------------------------------------------------------------------- */

/* Set what this address holds and keep a count per HID bit, so a key released elsewhere never removes these bits */
void KeyOverridesDygma::setModContribution(uint8_t offset, uint8_t hid)
{
  uint8_t old = mod_hid_[offset];

  if (old == hid) {
    return;
  }

  for (uint8_t bit = 0; bit < 8; ++bit) {
    uint8_t mask = (uint8_t)(1 << bit);

    if ((old & mask) && !(hid & mask)) {
      hid_count_[bit]--;
    } else if (!(old & mask) && (hid & mask)) {
      hid_count_[bit]++;
    }

    if (hid_count_[bit] != 0) {
      held_hid_ |= mask;
    } else {
      held_hid_ &= (uint8_t)~mask;
    }
  }

  mod_hid_[offset] = hid;
  setBit(mod_addr_, offset, hid != 0);
}

/* Held modifiers, side folded, without OneShot */
uint8_t KeyOverridesDygma::transitionMods() const
{
  return (uint8_t)(foldHid(held_hid_) | foldFlags(::LayerHoldMods.heldMods()));
}

/* Active OneShot modifiers count for matching, as QMK adds get_oneshot_mods(), but never as a modifier event */
uint8_t KeyOverridesDygma::oneShotMods()
{
  uint8_t hid = 0;

  for (uint8_t i = 0; i < 8; ++i) {
    if (OneShot::isModifierActive(Key((uint8_t)(HID_KEYBOARD_FIRST_MODIFIER + i), KEY_FLAGS))) {
      hid |= (uint8_t)(1 << i);
    }
  }

  return foldHid(hid);
}

bool KeyOverridesDygma::modsMatch(const override_t &o, uint8_t mods)
{
  if (o.negative_mods & mods) {
    return false;
  }

  if (o.trigger_mods == 0) {
    return true;
  }

  if (o.options & OPT_ONE_MOD) {
    return (o.trigger_mods & mods) != 0;
  }

  return (o.trigger_mods & mods) == o.trigger_mods;
}

/* No activation bit set means all of them, as QMK does */
bool KeyOverridesDygma::activationAllowed(uint8_t options, bool is_mod, bool key_down)
{
  uint8_t bits = options & OPT_ALL_ACTIVATIONS;

  if (bits == 0) {
    bits = OPT_ALL_ACTIVATIONS;
  }

  if (!is_mod) {
    return key_down && (bits & OPT_TRIGGER_DOWN);
  }

  return (bits & (key_down ? OPT_REQUIRED_MOD_DOWN : OPT_NEGATIVE_MOD_UP)) != 0;
}

/* ------------------------------------------------------------------------- */
/* Key events                                                                */
/* ------------------------------------------------------------------------- */

EventHandlerResult KeyOverridesDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  /* Address-less events (macros, OneShot) are never triggers, other keys or modifier sources */
  if ((count_ == 0 && !muted_any_) || !key_addr.isValid() || key_addr.toInt() >= MAX_TRACKED_KEYS) {
    return EventHandlerResult::OK;
  }

  uint8_t offset = key_addr.toInt();

  if (isModifier(mapped_key)) {
    uint8_t bits = modifierBits(mapped_key);

    if (keyToggledOff(key_state)) {
      /* Free the address first, also a trigger whose live cache entry a Superkey hold turned into a modifier */
      releaseAt(offset);

      if (count_ != 0) {
        /* Remove only this key's own bits. Other modifiers delivered here stay until a cycle passes without them */
        setModContribution(offset, (uint8_t)(mod_hid_[offset] & ~bits));
        modEvent(false);
      }

      return EventHandlerResult::OK;
    }

    if (count_ != 0 && keyIsPressed(key_state)) {
      /* The first modifier event at an address in a cycle replaces what it holds, later ones in the same cycle add to it */
      uint8_t kept = getBit(mod_seen_, offset) ? mod_hid_[offset] : (uint8_t)0;

      setBit(mod_seen_, offset, true);
      mod_seen_any_ = true;
      setModContribution(offset, (uint8_t)(kept | bits));

      /* Process every modifier press as an event, even when the folded mask does not change, as QMK does */
      if (keyToggledOn(key_state)) {
        modEvent(true);
      }
    }

    return EventHandlerResult::OK;
  }

  if (keyToggledOff(key_state)) {
    /* Resolve the release to what the address sent, so an injected release takes the right key out */
    EventHandlerResult result = outputAt(mapped_key, key_addr, key_state);

    if (active_valid_ && active_addr_ == offset) {
      sendFreshReplacement();
    }

    releaseAt(offset);
    return result;
  }

  if (count_ != 0 && keyToggledOn(key_state) && mapped_key != Key_NoKey) {
    /* Skip a Superkeys timeline or hold delivery that repeats a press the address already made */
    bool duplicate = (key_state & INJECTED) &&
                     (getBit(down_, offset) || getBit(muted_, offset) || (active_valid_ && active_addr_ == offset));

    if (!duplicate) {
      setBit(down_, offset, true);
      setBit(down_stale_, offset, false);
      down_any_ = true;
      pressAt(mapped_key, key_addr);
    }
  }

  return outputAt(mapped_key, key_addr, key_state);
}

/* A non-modifier key down, QMK last_key_down handling */
void KeyOverridesDygma::pressAt(Key key, KeyAddr key_addr)
{
  uint8_t keycode = key.getKeyCode();
  bool can_trigger = key.getFlags() == KEY_FLAGS && keycode >= HID_KEYBOARD_A_AND_A && keycode <= HID_LAST_KEY;

  last_addr_ = key_addr.toInt();
  last_keycode_ = can_trigger ? keycode : (uint8_t)0;
  last_layer_ = Layer.lookupActiveLayer(key_addr);
  last_down_ms_ = Runtime.millisAtCycleStart();

  /* Cancel a deferred register on every key down as QMK does. A waiting trigger stays muted */
  cancelDeferred();

  bool activated = can_trigger && tryActivate(false, true, (uint8_t)(transitionMods() | oneShotMods()));

  if (!activated && active_valid_ && !(active_options_ & OPT_NO_UNREGISTER_OTHER)) {
    deactivate(false);
  }

  /* Take in the LayerHoldMods modifiers registered with this press, and keep a loss no toggle reported for maskChanged */
  mods_last_ = (uint8_t)(mods_last_ | transitionMods());
}

void KeyOverridesDygma::releaseAt(uint8_t offset)
{
  if (active_valid_ && active_addr_ == offset) {
    deactivate(false);
  }

  if (last_addr_ == offset) {
    last_addr_ = NO_ADDR;
    cancelDeferred();
  }

  if (deferred_addr_ == offset) {
    cancelDeferred();
  }

  if (note_addr_ == offset) {
    note_addr_ = NO_ADDR;
  }

  setMuted(offset, false);
  setBit(down_, offset, false);
  setBit(down_stale_, offset, false);
}

EventHandlerResult KeyOverridesDygma::outputAt(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  uint8_t offset = key_addr.toInt();

  if (active_valid_ && active_addr_ == offset) {
    if (active_replacement_ == 0 || !active_registered_) {
      return EventHandlerResult::EVENT_CONSUMED;
    }

    /* The replacement's modifiers go out through the report filter, as QMK weak override mods */
    mapped_key = Key((uint8_t)(active_replacement_ & 0xFF), KEY_FLAGS);

    if (keyToggledOn(key_state)) {
      replacement_fresh_ = true;
    }
  } else if (getBit(muted_, offset)) {
    return EventHandlerResult::EVENT_CONSUMED;
  }

  /* A deferred register reaches the report without a toggle-on, so record it for previous-key rules here */
  if (note_addr_ == offset && keyIsPressed(key_state) && !keyToggledOn(key_state)) {
    note_addr_ = NO_ADDR;
    ::AltRepeatTrack.noteTyped(mapped_key, key_addr);
  }

  return EventHandlerResult::OK;
}

/* Send a replacement pressed in this cycle before the trigger's release takes it out, so a tap Qukeys replays within one cycle
 * still reaches the host once with its modifier filter */
void KeyOverridesDygma::sendFreshReplacement()
{
  if (!replacement_fresh_ || !active_registered_ || active_replacement_ == 0) {
    return;
  }

  uint8_t keycode = (uint8_t)(active_replacement_ & 0xFF);

  /* Skip it when a mid-cycle report already carried it */
  if (::Keyboard.isKeyPressed(keycode) && !::Keyboard.wasKeyPressed(keycode)) {
    Runtime.hid().keyboard().sendReport();
  }

  replacement_fresh_ = false;
}

/* ------------------------------------------------------------------------- */
/* Activation                                                                */
/* ------------------------------------------------------------------------- */

bool KeyOverridesDygma::tryActivate(bool is_mod, bool key_down, uint8_t mods)
{
  /* Leave the host report alone while the keyboard API holds key reports off */
  if (!::BaseDygma.isKeyReportEnabled()) {
    return false;
  }

  /* Only the last non-modifier key, still held, can be the trigger */
  if (last_addr_ == NO_ADDR || last_keycode_ == 0 || last_layer_ >= MAX_LAYERS) {
    return false;
  }

  for (uint8_t i = 0; i < count_; ++i) {
    const override_t &o = p_config_->overrides[i];

    if (!o.enabled || o.trigger != last_keycode_ || !(o.layers & (uint16_t)(1u << last_layer_))) {
      continue;
    }

    if (!activationAllowed(o.options, is_mod, key_down) || (active_valid_ && active_index_ == i) || !modsMatch(o, mods)) {
      continue;
    }

    activate(i, is_mod);
    return true;
  }

  return false;
}

void KeyOverridesDygma::activate(uint8_t index, bool via_modifier)
{
  const override_t &o = p_config_->overrides[index];

  deactivate(false);
  cancelDeferred();

  active_valid_ = true;
  active_index_ = index;
  active_addr_ = last_addr_;
  active_registered_ = !via_modifier;
  active_replacement_ = o.replacement;
  active_options_ = o.options;
  replacement_fresh_ = false;

  ::Keyboard.setModifierFilter(bothSides(o.suppressed_mods), flagsToHid((uint8_t)(o.replacement >> 8)));

  if (!via_modifier) {
    return;
  }

  /* Take the trigger out of the report being built. releaseKey also stops it counting as the last toggled-on keycode */
  Runtime.hid().keyboard().releaseKey(Key(o.trigger, KEY_FLAGS));

  if (o.replacement != 0) {
    scheduleDeferred(Deferred::REPLACEMENT, active_addr_);
  }
}

void KeyOverridesDygma::deactivate(bool allow_reregister)
{
  if (!active_valid_) {
    return;
  }

  uint8_t addr = active_addr_;
  bool reregister = allow_reregister && !(active_options_ & OPT_NO_REREGISTER) && getBit(down_, addr);

  active_valid_ = false;
  cancelDeferred();
  ::Keyboard.setModifierFilter(0, 0);

  if (active_registered_ && active_replacement_ != 0) {
    Runtime.hid().keyboard().releaseKey(Key((uint8_t)(active_replacement_ & 0xFF), KEY_FLAGS));
  }

  /* Keep the trigger silent until it is released or its deferred register fires */
  setMuted(addr, true);

  if (reregister) {
    scheduleDeferred(Deferred::TRIGGER, addr);
  }
}

/* One modifier toggle, QMK is_mod processing */
bool KeyOverridesDygma::modEvent(bool key_down)
{
  uint8_t effective = (uint8_t)(transitionMods() | oneShotMods());
  bool activated = tryActivate(true, key_down, effective);

  if (!activated && active_valid_ && !modsMatch(p_config_->overrides[active_index_], effective)) {
    deactivate(true);
  }

  mods_last_ = transitionMods();
  return activated;
}

/* Catch a modifier change no toggle reported, such as a LayerHoldMods drop or a contribution the sweep dropped */
void KeyOverridesDygma::maskChanged()
{
  uint8_t mods = transitionMods();
  uint8_t old = mods_last_;

  if (mods == old) {
    return;
  }

  if (last_addr_ == NO_ADDR && !active_valid_) {
    mods_last_ = mods;
    return;
  }

  bool activated = (mods & ~old) != 0 && modEvent(true);

  if (!activated && (old & ~mods) != 0) {
    modEvent(false);
  }

  mods_last_ = mods;
}

/* ------------------------------------------------------------------------- */
/* Deferred register                                                         */
/* ------------------------------------------------------------------------- */

/* Wait until the trigger was held for the repeat delay, or a short moment when that has passed, as QMK does */
void KeyOverridesDygma::scheduleDeferred(Deferred kind, uint8_t offset)
{
  uint32_t elapsed = Runtime.millisAtCycleStart() - last_down_ms_;
  uint32_t ms = SHORT_DELAY_MS;

  if (last_addr_ != NO_ADDR && elapsed < REPEAT_DELAY_MS) {
    ms = REPEAT_DELAY_MS - elapsed;
  }

  deferred_ = kind;
  deferred_addr_ = offset;
  kbdtimer_set_ms(&timer_, ms);
}

void KeyOverridesDygma::cancelDeferred()
{
  deferred_ = Deferred::NONE;
  deferred_addr_ = NO_ADDR;
}

void KeyOverridesDygma::fireDeferred()
{
  Deferred kind = deferred_;
  uint8_t addr = deferred_addr_;

  cancelDeferred();

  if (kind == Deferred::REPLACEMENT) {
    if (active_valid_ && active_addr_ == addr) {
      active_registered_ = true;
      note_addr_ = addr;
    }
  } else if (kind == Deferred::TRIGGER) {
    setMuted(addr, false);
    note_addr_ = addr;
  }
}

/* ------------------------------------------------------------------------- */
/* Cycle hooks                                                               */
/* ------------------------------------------------------------------------- */

EventHandlerResult KeyOverridesDygma::beforeEachCycle()
{
  /* The last report carried any replacement pressed in the cycle before */
  replacement_fresh_ = false;

  if (wake_pending_ && kbdtimer_check(&wake_)) {
    wake_pending_ = false;
  }

  if (count_ == 0 && !muted_any_) {
    return EventHandlerResult::OK;
  }

  /* Check every cycle while a deadline is pending, because the system timer keeps only two thresholds */
  if (deferred_ != Deferred::NONE && kbdtimer_check(&timer_)) {
    fireDeferred();
  }

  /* Treat a key that is no longer held as released when another plugin consumed its release. Run after the report, so an
   * injected trigger whose physical key is already up still reached one report */
  if (active_valid_ && !isHeld(active_addr_)) {
    deactivate(false);
  }

  if (last_addr_ != NO_ADDR && !isHeld(last_addr_)) {
    last_addr_ = NO_ADDR;
    cancelDeferred();
  }

  if (muted_any_ || down_any_ || held_hid_ != 0 || mod_seen_any_) {
    sweep();
  }

  return EventHandlerResult::OK;
}

EventHandlerResult KeyOverridesDygma::beforeReportingState()
{
  if (count_ == 0) {
    return EventHandlerResult::OK;
  }

  /* Send nothing of an override while the keyboard API holds key reports off, as BaseDygma empties the report after this hook */
  if (!::BaseDygma.isKeyReportEnabled()) {
    deactivate(false);
    cancelDeferred();
    mods_last_ = transitionMods();
    return EventHandlerResult::OK;
  }

  maskChanged();

  /* Wake once more when the trigger lost its key without a release, so the next cycle ends the override and sends a clean report */
  if (active_valid_ && !isHeld(active_addr_) && !wake_pending_) {
    kbdtimer_set_ms(&wake_, 1);
    wake_pending_ = true;
  }

  return EventHandlerResult::OK;
}

/* Retire state of the cycle that just ended. Walk only the bitmap bytes that have a bit set */
void KeyOverridesDygma::sweep()
{
  bool muted_any = false;
  bool down_any = false;

  for (uint8_t byte = 0; byte < MAX_TRACKED_KEYS / 8; ++byte) {
    uint8_t live = (uint8_t)(mod_addr_[byte] | muted_[byte] | down_[byte]);

    if (live == 0) {
      continue;
    }

    for (uint8_t bit = 0; bit < 8; ++bit) {
      uint8_t mask = (uint8_t)(1 << bit);

      if (!(live & mask)) {
        continue;
      }

      uint8_t offset = (uint8_t)(byte * 8 + bit);
      bool held = isHeld(offset);

      /* Drop modifiers an address did not deliver in the last cycle, or holds without its key, such as a released Superkey's
       * cached modifiers */
      if ((mod_addr_[byte] & mask) && (!(mod_seen_[byte] & mask) || !held)) {
        setModContribution(offset, 0);
      }

      if ((muted_[byte] & mask) && !held) {
        muted_[byte] &= (uint8_t)~mask;

        if (deferred_ == Deferred::TRIGGER && deferred_addr_ == offset) {
          cancelDeferred();
        }
      }

      /* Retire a press after two sweeps in a row without its key, so a late Superkeys duplicate still finds it */
      if (down_[byte] & mask) {
        if (held || (active_valid_ && active_addr_ == offset)) {
          down_stale_[byte] &= (uint8_t)~mask;
        } else if (down_stale_[byte] & mask) {
          down_[byte] &= (uint8_t)~mask;
          down_stale_[byte] &= (uint8_t)~mask;
        } else {
          down_stale_[byte] |= mask;
        }
      }
    }

    if (muted_[byte] != 0) {
      muted_any = true;
    }

    if (down_[byte] != 0) {
      down_any = true;
    }
  }

  memset(mod_seen_, 0, sizeof(mod_seen_));
  mod_seen_any_ = false;
  muted_any_ = muted_any;
  down_any_ = down_any;
}

/* End the override and every pending register. Clear the modifier tracking too when no override is left to use it */
void KeyOverridesDygma::endAll(bool clear_mods)
{
  /* Release the replacement and mute the trigger until its release, with no synthesized press */
  deactivate(false);
  cancelDeferred();
  note_addr_ = NO_ADDR;
  last_addr_ = NO_ADDR;
  ::Keyboard.setModifierFilter(0, 0);

  if (!clear_mods) {
    return;
  }

  memset(mod_hid_, 0, sizeof(mod_hid_));
  memset(mod_addr_, 0, sizeof(mod_addr_));
  memset(mod_seen_, 0, sizeof(mod_seen_));
  memset(hid_count_, 0, sizeof(hid_count_));
  memset(down_, 0, sizeof(down_));
  memset(down_stale_, 0, sizeof(down_stale_));
  mod_seen_any_ = false;
  down_any_ = false;
  held_hid_ = 0;
  mods_last_ = 0;
}

/* ------------------------------------------------------------------------- */
/* Override values                                                           */
/* ------------------------------------------------------------------------- */

bool KeyOverridesDygma::overrideAllowed(const override_t &o)
{
  if (o.trigger < HID_KEYBOARD_A_AND_A || o.trigger > HID_LAST_KEY) {
    return false;
  }

  if ((o.trigger_mods | o.negative_mods | o.suppressed_mods) & (uint8_t)~MODS_MASK) {
    return false;
  }

  if (o.trigger_mods & o.negative_mods) {
    return false;
  }

  if (o.layers == 0 || (o.layers & (uint16_t)~LAYERS_MASK) != 0) {
    return false;
  }

  if ((o.options & (uint8_t)~OPTIONS_MASK) != 0 || o.enabled > 1) {
    return false;
  }

  if (o.replacement == 0) {
    return true;
  }

  uint8_t flags = (uint8_t)(o.replacement >> 8);
  uint8_t keycode = (uint8_t)(o.replacement & 0xFF);

  return (flags & (uint8_t)~REPLACEMENT_MODS) == 0 && keycode >= HID_KEYBOARD_A_AND_A && keycode <= HID_LAST_KEY;
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult KeyOverridesDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, "keyoverrides.map")) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "keyoverrides.map") != 0) {
    return EventHandlerResult::OK;
  }

  if (::Focus.isEOL()) {
    sendOverrides();
  } else if (readOverrides()) {
    /* End the old override before the save, since its index refers to the old table */
    endAll(staging_.count == 0);
    cfgmem_config_save(&staging_);
    count_ = configValid() ? p_config_->count : 0;
  } else {
    /* Read the rest of a rejected line here, because FocusSerial::drain does not wait for bytes still in transit */
    for (int c = AltRepeatDygma::peekWait(); c >= 0 && c != '\n'; c = AltRepeatDygma::peekWait()) {
      Runtime.serialPort().read();
    }

    ::Focus.send("error");
  }

  /* Keep the old table on a rejected write and leave the newline for FocusSerial */
  return EventHandlerResult::EVENT_CONSUMED;
}

void KeyOverridesDygma::sendOverrides() const
{
  if (!configValid()) {
    ::Focus.send((uint8_t)0);
    return;
  }

  ::Focus.send(p_config_->count);

  for (uint8_t i = 0; i < p_config_->count; ++i) {
    const override_t &o = p_config_->overrides[i];

    ::Focus.send(o.trigger, o.trigger_mods, o.layers, o.negative_mods, o.suppressed_mods, o.replacement, o.options, o.enabled);
  }
}

/* Parse a whole table into staging_. Return false on the first missing, malformed or out of range value */
bool KeyOverridesDygma::readOverrides()
{
  memset(&staging_, 0, sizeof(staging_));
  staging_.layout_version = LAYOUT_VERSION;

  uint16_t count = 0;

  if (!AltRepeatDygma::readValue(count) || count > MAX_OVERRIDES) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t v[8];

    for (uint8_t j = 0; j < 8; ++j) {
      if (!AltRepeatDygma::readValue(v[j])) {
        return false;
      }
    }

    /* Reject byte fields above 0xFF before they are narrowed. Layers and replacement are 16 bit */
    if (v[0] > 0xFF || v[1] > 0xFF || v[3] > 0xFF || v[4] > 0xFF || v[6] > 0xFF || v[7] > 0xFF) {
      return false;
    }

    override_t &o = staging_.overrides[i];

    o.trigger = (uint8_t)v[0];
    o.trigger_mods = (uint8_t)v[1];
    o.layers = v[2];
    o.negative_mods = (uint8_t)v[3];
    o.suppressed_mods = (uint8_t)v[4];
    o.replacement = v[5];
    o.options = (uint8_t)v[6];
    o.enabled = (uint8_t)v[7];

    if (!overrideAllowed(o)) {
      return false;
    }
  }

  int c = AltRepeatDygma::peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = AltRepeatDygma::peekWait();
  }

  if (c != '\n') {
    return false;
  }

  staging_.count = (uint8_t)count;
  return true;
}

void KeyOverridesDygma::cfgmem_config_save(const keyoverrides_config_t *p_config)
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_data_save( p_config_, p_config, sizeof(keyoverrides_config_t) );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

  UNUSED( result );
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::KeyOverridesDygma KeyOverrides;
