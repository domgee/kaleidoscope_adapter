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

#include "LayerHoldModsDygma.h"

#include "AltRepeatDygma.h"
#include "CombosDygma.h"
#include "ConditionalLayersDygma.h"
#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"
#include "kaleidoscope/layers.h"

namespace kaleidoscope {
namespace plugin {

static const struct {
  uint8_t flag;
  Key key;
} MOD_KEYS[] = {
  {CTRL_HELD, Key_LeftControl}, {LALT_HELD, Key_LeftAlt}, {RALT_HELD, Key_RightAlt},
  {SHIFT_HELD, Key_LeftShift}, {GUI_HELD, Key_LeftGui},
};

EventHandlerResult LayerHoldModsDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state) {
  if (!isHoldKey(mapped_key)) {
    if (keyToggledOff(key_state)) {
      setBit(bare_, key_addr, false);
      setBit(delivered_, key_addr, false);
    }

    return EventHandlerResult::OK;
  }

  Key hold_key = mapped_key;

  /* Register first, so the form sent below follows the outcome. Keep the first delivery's form for an injected duplicate */
  if (keyToggledOn(key_state) && key_addr.isValid() && !((key_state & INJECTED) && getBit(delivered_, key_addr))) {
    if (registerHold(hold_key, key_addr)) {
      setBit(bare_, key_addr, true);
      /* Press now so a mid-scan report from an Autoshift tap carries them */
      pressMods(held_mods_);
    } else {
      /* Send a press that did not register as a plain shortcut */
      setBit(bare_, key_addr, false);
    }

    setBit(delivered_, key_addr, true);
  }

  /* Send a registered press as the bare key so the layer alone decides how long the modifiers last */
  uint8_t flags = getBit(bare_, key_addr) ? (uint8_t)0 : (uint8_t)(hold_key.getFlags() & ~HOLD_FLAG);
  mapped_key = Key(hold_key.getKeyCode(), flags);

  if (keyToggledOn(key_state) && key_addr.isValid()) {
    Layer.updateLiveCompositeKeymap(key_addr, mapped_key);
  }

  if (keyToggledOff(key_state)) {
    setBit(bare_, key_addr, false);
    setBit(delivered_, key_addr, false);
  }

  return EventHandlerResult::OK;
}

bool LayerHoldModsDygma::registerHold(Key key, KeyAddr key_addr) {
  uint8_t mods = key.getFlags() & MODS_MASK;
  uint8_t keycode = key.getKeyCode();

  if (mods == 0 || keycode < HID_KEYBOARD_A_AND_A || keycode > HID_LAST_KEY) {
    return false;
  }

  uint8_t owner = Layer.lookupActiveLayer(key_addr);

  if (owner == 0 || owner >= MAX_LAYERS) {
    return false;
  }

  /* Let only the owner layer's keymap, or a previous-key rule on the owner layer whose output this press is, define the key.
   * Keep Superkey actions, combo outputs, stale replays and the default layer plain */
  if (Layer.getKey(owner, key_addr).getRaw() != key.getRaw() && !AltRepeat.ruleOutputAt(key_addr, key, owner)) {
    return false;
  }

  mods_[owner] |= mods;
  owners_ |= (uint32_t)1 << owner;
  held_mods_ |= mods;
  return true;
}

bool LayerHoldModsDygma::ownerInactive() const {
  for (uint8_t layer = 1; layer < MAX_LAYERS; layer++) {
    if ((owners_ & ((uint32_t)1 << layer)) && !Layer.isActive(layer)) {
      return true;
    }
  }

  return false;
}

EventHandlerResult LayerHoldModsDygma::onLayerChange() {
  if (owners_ == 0 || !ownerInactive()) {
    return EventHandlerResult::OK;
  }

  /* Apply in the report hooks, so a deactivate and reactivate in one scan keeps the modifiers */
  check_ = true;

  /* The report is already built (Focus layer commands, OneShot afterEachCycle). Wake once more to send the release */
  if (report_built_ && !wake_pending_) {
    kbdtimer_set_ms(&wake_timer_, 1);
    wake_pending_ = true;
  }

  return EventHandlerResult::OK;
}

EventHandlerResult LayerHoldModsDygma::beforeEachCycle() {
  report_built_ = false;

  if (wake_pending_ && kbdtimer_check(&wake_timer_)) {
    wake_pending_ = false;
  }

  if (owners_ == 0) {
    return EventHandlerResult::OK;
  }

  /* Skip the release here, because releaseAllKeys emptied the report */
  if (check_) {
    dropInactiveOwners(false);
  }

  /* Press before the scan so mid-scan reports (Autoshift tap, macros) carry the modifiers */
  pressMods(held_mods_);
  return EventHandlerResult::OK;
}

EventHandlerResult LayerHoldModsDygma::beforeReportingState() {
  report_built_ = true;

  if (owners_ == 0) {
    return EventHandlerResult::OK;
  }

  if (check_) {
    dropInactiveOwners(true);
  }

  /* Press again after the scan, because an injected modifier release strips the shared bit */
  pressMods(held_mods_);
  return EventHandlerResult::OK;
}

void LayerHoldModsDygma::dropInactiveOwners(bool in_report) {
  check_ = false;
  uint8_t before = held_mods_;
  held_mods_ = 0;
  bool shifts_read = false;
  uint32_t shifted = 0;

  for (uint8_t layer = 1; layer < MAX_LAYERS; layer++) {
    uint32_t bit = (uint32_t)1 << layer;

    if (!(owners_ & bit)) {
      continue;
    }

    if (!Layer.isActive(layer)) {
      if (!shifts_read) {
        shifted = heldLayerShifts();
        /* Add the layers the conditional layer rules derive from the active and held layers, since they come back with them */
        shifted |= ConditionalLayers.derived((uint16_t)((ConditionalLayersDygma::activeMask() | shifted) & ConditionalLayersDygma::LAYERS_MASK));
        shifts_read = true;
      }

      if (!(shifted & bit)) {
        owners_ &= ~bit;
        mods_[layer] = 0;
        continue;
      }

      /* Keep the hold while a held momentary key or a rule on held layers brings the layer back on its next event, so check again */
      check_ = true;
    }

    held_mods_ |= mods_[layer];
  }

  uint8_t dropped = before & ~held_mods_;

  if (in_report && dropped != 0) {
    releaseMods(dropped);
    /* Put back the physically held modifiers, because a release strips the modifier for every key */
    pressPhysicalModifiers();
  }
}

void LayerHoldModsDygma::pressMods(uint8_t mods) {
  for (auto &mod : MOD_KEYS) {
    if (mods & mod.flag) {
      Runtime.hid().keyboard().pressKey(mod.key, false);
    }
  }
}

void LayerHoldModsDygma::releaseMods(uint8_t mods) {
  for (auto &mod : MOD_KEYS) {
    if (mods & mod.flag) {
      Runtime.hid().keyboard().releaseKey(mod.key);
    }
  }
}

/* Return the layers that a pressed momentary layer key shifts to */
uint32_t LayerHoldModsDygma::heldLayerShifts() {
  uint32_t layers = 0;

  for (auto key_addr : KeyAddr::all()) {
    /* Skip a key whose holds Combos swallows, such as a spent anchor, because it never reactivates its layer */
    if (!Runtime.device().isKeyswitchPressed(key_addr) || Runtime.device().isKeyMasked(key_addr) || Combos.swallowsHolds(key_addr)) {
      continue;
    }

    /* Match ShiftToLayer(n) in the live cache, which also holds a resolved dual-function or Superkey layer hold */
    Key key = Layer.lookup(key_addr);
    uint8_t keycode = key.getKeyCode();

    if (key.getFlags() != (KEY_FLAGS | SYNTHETIC | SWITCH_TO_KEYMAP) || keycode < LAYER_SHIFT_OFFSET ||
        keycode >= LAYER_SHIFT_OFFSET + MAX_LAYERS) {
      continue;
    }

    layers |= (uint32_t)1 << (keycode - LAYER_SHIFT_OFFSET);
  }

  return layers;
}

/* Leave OneShot modifiers to OneShot::beforeReportingState, which runs after this plugin */
void LayerHoldModsDygma::pressPhysicalModifiers() {
  for (auto key_addr : KeyAddr::all()) {
    if (!Runtime.device().isKeyswitchPressed(key_addr) || Runtime.device().isKeyMasked(key_addr)) {
      continue;
    }

    /* Read the live cache, which holds what the key resolved to, so qukey and superkey hold modifiers count too */
    Key key = Layer.lookup(key_addr);

    if ((key.getFlags() & (SYNTHETIC | RESERVED)) || key.getKeyCode() < HID_KEYBOARD_FIRST_MODIFIER ||
        key.getKeyCode() > HID_KEYBOARD_LAST_MODIFIER) {
      continue;
    }

    Runtime.hid().keyboard().pressKey(key, false);
  }
}

bool LayerHoldModsDygma::getBit(const uint8_t *bits, KeyAddr key_addr) {
  if (!key_addr.isValid()) {
    return false;
  }

  uint8_t offset = key_addr.toInt();

  if (offset >= MAX_TRACKED_KEYS) {
    return false;
  }

  return (bits[offset >> 3] & (1 << (offset & 7))) != 0;
}

void LayerHoldModsDygma::setBit(uint8_t *bits, KeyAddr key_addr, bool value) {
  if (!key_addr.isValid()) {
    return;
  }

  uint8_t offset = key_addr.toInt();

  if (offset >= MAX_TRACKED_KEYS) {
    return;
  }

  if (value) {
    bits[offset >> 3] |= (1 << (offset & 7));
  } else {
    bits[offset >> 3] &= ~(1 << (offset & 7));
  }
}

EventHandlerResult LayerHoldModsDygma::onFocusEvent(const char *command) {
  if (::Focus.handleHelp(command, "layerhold.state")) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "layerhold.state") != 0) {
    return EventHandlerResult::OK;
  }

  /* Report only. Bazecor offers the checkbox when help lists this command */
  ::Focus.send(owners_, held_mods_);
  return EventHandlerResult::EVENT_CONSUMED;
}

}
}

kaleidoscope::plugin::LayerHoldModsDygma LayerHoldMods;
