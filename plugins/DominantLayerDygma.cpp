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

#include "DominantLayerDygma.h"
#include "kbdfal_ll_memory.h"

#include "AltRepeatDygma.h"
#include "AutoshiftDygma.h"
#include "CombosDygma.h"
#include "ConditionalLayersDygma.h"
#include "DynamicMacrosDygma.h"
#include "EEPROMKeymapDygma.h"
#include "Kaleidoscope-FocusSerial.h"
#include "KeyRoleManager.h"
#include "QukeysDygma.h"
#include "SuperkeysHandler.h"
#include "SwapHandsDygma.h"
#include <Kaleidoscope-Ranges.h>
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"
#include "kaleidoscope/layers.h"

namespace kaleidoscope {
namespace plugin {

DominantLayerDygma::dominant_config_t DominantLayerDygma::staging_;

EventHandlerResult DominantLayerDygma::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_DOMINANT, (const void **)&p_config_ );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Write the defaults over erased flash or another layout, which happens once after a flash */
  if (!configValid()) {
    stageCurrent();
    cfgmem_config_save(&staging_);
  }

  UNUSED( result );

  return EventHandlerResult::OK;
}

bool DominantLayerDygma::flagGet(const uint8_t *map, KeyAddr key_addr)
{
  uint8_t offset = key_addr.toInt();
  return key_addr.isValid() && offset < POSITIONS && (map[offset >> 3] & (1u << (offset & 7))) != 0;
}

void DominantLayerDygma::flagSet(uint8_t *map, KeyAddr key_addr, bool value)
{
  uint8_t offset = key_addr.toInt();

  if (!key_addr.isValid() || offset >= POSITIONS) {
    return;
  }

  if (value) {
    map[offset >> 3] |= (uint8_t)(1u << (offset & 7));
  } else {
    map[offset >> 3] &= (uint8_t)~(1u << (offset & 7));
  }
}

bool DominantLayerDygma::configValid() const
{
  return p_config_ != nullptr && p_config_->layout_version == LAYOUT_VERSION;
}

uint16_t DominantLayerDygma::timeoutMs() const
{
  if (!configValid() || p_config_->timeout_ms < TIMEOUT_MIN_MS || p_config_->timeout_ms > TIMEOUT_MAX_MS) {
    return TIMEOUT_DEFAULT_MS;
  }

  return p_config_->timeout_ms;
}

uint8_t DominantLayerDygma::layerOf(uint8_t position) const
{
  if (!configValid() || position >= POSITIONS || (position >= GAP_FIRST && position <= GAP_LAST)) {
    return NO_LAYER;
  }

  uint8_t layer = p_config_->layer[position];

  return layer >= MIN_LAYER && layer <= MAX_LAYER ? layer : NO_LAYER;
}

uint8_t DominantLayerDygma::layerAt(KeyAddr key_addr) const
{
  return key_addr.isValid() ? layerOf(key_addr.toInt()) : NO_LAYER;
}

/* ------------------------------------------------------------------------- */
/* Key handling                                                              */
/* ------------------------------------------------------------------------- */

bool DominantLayerDygma::isLayerKey(Key key)
{
  return key.getFlags() == (SYNTHETIC | SWITCH_TO_KEYMAP);
}

/* Return true for a key that can change the layers itself. Layer 0 never holds such a key back */
bool DominantLayerDygma::changesLayer(Key key)
{
  uint16_t raw = key.getRaw();

  if (isLayerKey(key) || (raw >= ranges::OSL_FIRST && raw <= ranges::OSL_LAST) || (raw >= ranges::DUL_FIRST && raw <= ranges::DUL_LAST)) {
    return true;
  }

  Key tap;
  Key hold;

  if (keyRoleManager.dual_function_superkey(key, tap, hold) && isLayerKey(hold)) {
    return true;
  }

  return SwapHandsDygma::isSwapCode(raw);
}

/* Return true for a physical event, which excludes injected keys and the replays of Qukeys, Combos and SwapHands */
bool DominantLayerDygma::genuine(KeyAddr key_addr, uint8_t key_state) const
{
  return (key_state & INJECTED) == 0 && key_addr.isValid() && !keyRoleManager.isReplayingQueuedEvent() && !::Combos.isReemitting() &&
         !::SwapHands.isTapAt(key_addr);
}

bool DominantLayerDygma::canArm(KeyAddr key_addr) const
{
  if (!Runtime.device().isKeyswitchPressed(key_addr) || flagGet(passed_, key_addr)) {
    return false;
  }

  const EEPROMKeymap::keymap_config_t *p_keymap = EEPROMKeymap::getKeymapConfig();

  if (p_keymap == nullptr || p_keymap->ignore_hardcoded_layers == 0) {
    return false;
  }

  uint8_t layer = layerAt(key_addr);

  if (layer == NO_LAYER || Layer.lookupActiveLayer(key_addr) != 0 || Layer.isActive(layer)) {
    return false;
  }

  /* A replayed swap control never enables swap */
  Key cell = Layer.getKey(layer, key_addr);

  if (cell == Key_Transparent || SwapHandsDygma::isSwapCode(cell.getRaw()) || changesLayer(Layer.lookupOnActiveLayer(key_addr))) {
    return false;
  }

  /* Require an empty Qukeys queue, because a queued release can change the layers before a predicted layer key resolves */
  if (qukeys.hasPendingQukey() || qukeys.hasQueuedEvents()) {
    return false;
  }

  /* Leave a pending Autoshift key to settle as a tap at this physical press */
  return !SuperkeysHandler::hasPendingSuperkey() && !::Combos.isCollecting() && !::Autoshift.hasPendingKey();
}

/* Return true when the layer key at key_addr turns the dominant layer on at its press */
bool DominantLayerDygma::predictsDominant(KeyAddr key_addr, Key key) const
{
  /* A previous-key rule may replace the layer key on its replay */
  if (::AltRepeat.hasRulesAt(key_addr)) {
    return false;
  }

  uint16_t raw = key.getRaw();
  uint16_t active = ConditionalLayersDygma::activeMask();
  uint16_t target = 0;

  if (raw >= ranges::OSL_FIRST && raw <= ranges::OSL_LAST) {
    target = active | (uint16_t)(1u << (raw - ranges::OSL_FIRST));
  } else if (isLayerKey(key)) {
    uint8_t code = key.getKeyCode();

    if (code >= LAYER_MOVE_OFFSET) {
      uint8_t layer = code - LAYER_MOVE_OFFSET;

      if (layer >= ConditionalLayersDygma::MAX_LAYERS) {
        return false;
      }

      target = (uint16_t)(1u << layer);
    } else if (code >= LAYER_SHIFT_OFFSET) {
      uint8_t layer = code - LAYER_SHIFT_OFFSET;

      /* Next and previous fall out here */
      if (layer >= ConditionalLayersDygma::MAX_LAYERS) {
        return false;
      }

      target = active | (uint16_t)(1u << layer);
    } else {
      /* A lock on an active layer turns it off */
      if (code >= ConditionalLayersDygma::MAX_LAYERS || Layer.isActive(code)) {
        return false;
      }

      target = active | (uint16_t)(1u << code);
    }
  } else {
    return false;
  }

  return (::ConditionalLayers.derived(target) & (1u << layer_)) != 0;
}

/* Settle a wait whose dominant layer turned on or whose deadline passed. An expired or equal deadline gives layer 0 */
bool DominantLayerDygma::settleIfDue()
{
  if (state_ != State::WAITING) {
    return false;
  }

  if (dominant_due_) {
    resolve(Kind::DOMINANT);
    return true;
  }

  if (kbdtimer_check(&timer_)) {
    resolve(Kind::DEFAULT);
    return true;
  }

  return false;
}

void DominantLayerDygma::settle()
{
  if (state_ == State::WAITING) {
    resolve(Kind::DEFAULT);
  }
}

/* End the wait and replay the press once. Qukeys pins the replay to the physical press time and to the chosen layer's key */
void DominantLayerDygma::resolve(Kind kind)
{
  state_ = State::IDLE;

  /* A zeroed timer keeps its hardware threshold, which costs at most one empty wake */
  timer_ = 0;
  wake_ = 0;
  wake_armed_ = false;
  dominant_due_ = false;
  flagSet(passed_, addr_, true);

  bool dominant = kind == Kind::DOMINANT;
  Key pin = dominant ? Layer.getKey(layer_, addr_) : k0_;

  /* Play nothing for an empty cell, because a replay of Key_NoKey would resolve the address again. Still count the press for quick tap
   * and the macro stop */
  if (pin == Key_NoKey || pin == Key_Transparent) {
    Layer.updateLiveCompositeKeymap(addr_, Key_NoKey);
    qukeys.notePhysicalPress(addr_);
    ::DynamicMacros.notePhysicalPress(addr_);
    flagSet(silenced_, addr_, true);
    return;
  }

  qukeys.setPressOrigin(addr_, press_ms_, pin, dominant ? layer_ : 0, dominant);

  emitting_ = true;
  handleKeyswitchEvent(pin, addr_, IS_PRESSED);
  emitting_ = false;
}

EventHandlerResult DominantLayerDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  if (emitting_ || !genuine(key_addr, key_state)) {
    return EventHandlerResult::OK;
  }

  /* KD resolved this event before the hooks, so refresh it when the replay at its own address went out first */
  if (settleIfDue() && key_addr == addr_) {
    mapped_key = Layer.lookup(key_addr);
  }

  /* Forget an origin left pending when Combos took the replay as a member and swallowed its release */
  if (keyToggledOn(key_state)) {
    qukeys.dropPendingOrigin(key_addr);
  }

  if (state_ == State::WAITING) {
    if (key_addr == addr_) {
      if (!keyToggledOff(key_state)) {
        return EventHandlerResult::EVENT_CONSUMED;
      }

      resolve(Kind::DEFAULT);
      mapped_key = Layer.lookup(key_addr);
    } else if (keyToggledOn(key_state)) {
      /* Send the layer key first, so Qukeys queues it ahead of the replay and the replay meets the dominant layer at the queue head */
      if (predictsDominant(key_addr, mapped_key)) {
        emitting_ = true;
        handleKeyswitchEvent(mapped_key, key_addr, key_state);
        emitting_ = false;

        resolve(Kind::DOMINANT);
        flagSet(passed_, key_addr, true);
        return EventHandlerResult::EVENT_CONSUMED;
      }

      resolve(Kind::DEFAULT);
    } else if (keyToggledOff(key_state)) {
      resolve(Kind::DEFAULT);
    }
  }

  if (keyToggledOff(key_state) && !Runtime.device().isKeyswitchPressed(key_addr)) {
    bool was_silenced = flagGet(silenced_, key_addr);

    flagSet(silenced_, key_addr, false);
    flagSet(passed_, key_addr, false);

    return was_silenced ? EventHandlerResult::EVENT_CONSUMED : EventHandlerResult::OK;
  }

  if (flagGet(silenced_, key_addr) && !keyToggledOn(key_state)) {
    return EventHandlerResult::EVENT_CONSUMED;
  }

  if (!keyToggledOn(key_state)) {
    return EventHandlerResult::OK;
  }

  if (state_ == State::IDLE && canArm(key_addr)) {
    addr_ = key_addr;
    k0_ = Layer.lookupOnActiveLayer(key_addr);
    layer_ = layerAt(key_addr);
    press_ms_ = Runtime.millisAtCycleStart();

    dominant_due_ = false;
    wake_armed_ = false;
    wake_ = 0;

    /* Keep holds and a release from resolving to the key while it waits */
    Layer.updateLiveCompositeKeymap(key_addr, Key_NoKey);
    qukeys.capturePriorEligibility(key_addr);

    kbdtimer_set_ms(&timer_, timeoutMs());
    state_ = State::WAITING;

    return EventHandlerResult::EVENT_CONSUMED;
  }

  flagSet(passed_, key_addr, true);
  return EventHandlerResult::OK;
}

EventHandlerResult DominantLayerDygma::beforeReportingState()
{
  if (state_ != State::WAITING) {
    return EventHandlerResult::OK;
  }

  /* Check the wake every cycle while it is pending, so the system timer keeps its threshold */
  if (wake_armed_) {
    kbdtimer_check(&wake_);
  }

  settleIfDue();

  return EventHandlerResult::OK;
}

/* Latch a dominant layer that turned on before the deadline. Replay on the next event or report, never inside a Qukeys flush */
EventHandlerResult DominantLayerDygma::onLayerChange()
{
  if (state_ != State::WAITING || !Layer.isActive(layer_) || kbdtimer_check(&timer_)) {
    return EventHandlerResult::OK;
  }

  dominant_due_ = true;

  /* Wake for one more cycle, because the change may come after this plugin's report hook */
  if (!wake_armed_) {
    kbdtimer_set_ms(&wake_, 1);
    wake_armed_ = true;
  }

  return EventHandlerResult::OK;
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult DominantLayerDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, PSTR("dominant.map\ndominant.timeout"))) {
    return EventHandlerResult::OK;
  }

  /* Replay a waiting key before a firmware update. Leave the command to its owner */
  if (strncmp_P(command, PSTR("upgrade."), 8) == 0) {
    settle();
    return EventHandlerResult::OK;
  }

  if (strncmp_P(command, PSTR("dominant."), 9) != 0) {
    return EventHandlerResult::OK;
  }

  bool is_map = strcmp_P(command + 9, PSTR("map")) == 0;

  if (!is_map && strcmp_P(command + 9, PSTR("timeout")) != 0) {
    return EventHandlerResult::OK;
  }

  if (::Focus.isEOL()) {
    if (is_map) {
      sendMap();
    } else {
      ::Focus.send(timeoutMs());
    }

    return EventHandlerResult::EVENT_CONSUMED;
  }

  settle();

  if (is_map ? readMap() : readTimeout()) {
    cfgmem_config_save(&staging_);
  } else {
    drainLine();
    ::Focus.send("error");
  }

  /* Keep the old block on a rejected write and leave the newline for FocusSerial */
  return EventHandlerResult::EVENT_CONSUMED;
}

/* Copy the current block into staging_, or the defaults when it is invalid */
void DominantLayerDygma::stageCurrent()
{
  if (configValid()) {
    memcpy(&staging_, p_config_, sizeof(staging_));
    staging_.timeout_ms = timeoutMs();
    return;
  }

  memset(&staging_, NO_LAYER, sizeof(staging_));
  staging_.layout_version = LAYOUT_VERSION;
  staging_.reserved = 0;
  staging_.timeout_ms = TIMEOUT_DEFAULT_MS;
}

/* Send the count, then position and layer for each key with a dominant layer, in ascending position order */
void DominantLayerDygma::sendMap() const
{
  uint8_t count = 0;

  for (uint8_t pos = 0; pos < POSITIONS; ++pos) {
    if (layerOf(pos) != NO_LAYER) {
      ++count;
    }
  }

  ::Focus.send(count);

  for (uint8_t pos = 0; pos < POSITIONS; ++pos) {
    uint8_t layer = layerOf(pos);

    if (layer != NO_LAYER) {
      ::Focus.send(pos, layer);
    }
  }
}

/* Parse a whole map into staging_, keeping the timeout. Return false on the first missing, malformed, out of range or duplicate value */
bool DominantLayerDygma::readMap()
{
  stageCurrent();
  memset(staging_.layer, NO_LAYER, sizeof(staging_.layer));

  uint16_t count = 0;

  if (!AltRepeatDygma::readValue(count) || count > MAX_ENTRIES) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t pos = 0;
    uint16_t layer = 0;

    if (!AltRepeatDygma::readValue(pos) || !AltRepeatDygma::readValue(layer)) {
      return false;
    }

    if (pos >= POSITIONS || (pos >= GAP_FIRST && pos <= GAP_LAST) || layer < MIN_LAYER || layer > MAX_LAYER) {
      return false;
    }

    if (staging_.layer[pos] != NO_LAYER) {
      return false;
    }

    staging_.layer[pos] = (uint8_t)layer;
  }

  return readLineEnd();
}

/* Parse a timeout into staging_, keeping the map */
bool DominantLayerDygma::readTimeout()
{
  stageCurrent();

  uint16_t timeout = 0;

  if (!AltRepeatDygma::readValue(timeout) || timeout < TIMEOUT_MIN_MS || timeout > TIMEOUT_MAX_MS) {
    return false;
  }

  staging_.timeout_ms = timeout;

  return readLineEnd();
}

/* Return true when only spaces are left before the end of the line */
bool DominantLayerDygma::readLineEnd()
{
  int c = AltRepeatDygma::peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = AltRepeatDygma::peekWait();
  }

  return c == '\n';
}

/* Read the rest of a rejected line here, because FocusSerial::drain does not wait for bytes still in transit */
void DominantLayerDygma::drainLine()
{
  for (int c = AltRepeatDygma::peekWait(); c >= 0 && c != '\n'; c = AltRepeatDygma::peekWait()) {
    Runtime.serialPort().read();
  }
}

void DominantLayerDygma::cfgmem_config_save(const dominant_config_t *p_config)
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_data_save( p_config_, p_config, sizeof(dominant_config_t) );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

  UNUSED( result );
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::DominantLayerDygma DominantLayer;
