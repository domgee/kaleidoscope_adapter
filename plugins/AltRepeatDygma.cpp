/* -*- mode: c++ -*-
 * Kaleidoscope-AltRepeatDygma -- Previous-key alternates for Dygma keyboards
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

#include "AltRepeatDygma.h"
#include "kbdfal_ll_memory.h"

#include "CombosDygma.h"
#include "HIDReportObserver.h"
#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"
#include "kaleidoscope/layers.h"
#include "KeyboardioHID.h"
#include "KeyRoleManager.h"
#include "QukeysDygma.h"

namespace kaleidoscope {
namespace plugin {

AltRepeatDygma::altrepeat_config_t AltRepeatDygma::staging_;

/* Fold AltGr into Alt and keep Ctrl, Alt, Shift and Gui */
static uint8_t foldFlags(uint8_t flags)
{
  if (flags & RALT_HELD) {
    flags |= LALT_HELD;
  }

  return flags & AltRepeatDygma::MODS_MASK;
}

/* ------------------------------------------------------------------------- */
/* Substitution                                                              */
/* ------------------------------------------------------------------------- */

void AltRepeatDygma::repairCache(KeyAddr key_addr, uint8_t key_state)
{
  if (!keyToggledOn(key_state) || (key_state & INJECTED) || !key_addr.isValid() || keyRoleManager.isReplayingQueuedEvent()) {
    return;
  }

  uint8_t offset = key_addr.toInt();

  if (offset >= MAX_TRACKED_KEYS || active_out_[offset] == Key_NoKey) {
    return;
  }

  /* The earlier press's release still waits in the Qukeys queue, and its restore loop must keep holding what the host got */
  if (qukeys.hasQueuedReleaseFirst(key_addr)) {
    Layer.updateLiveCompositeKeymap(key_addr, active_out_[offset]);
  }
}

EventHandlerResult AltRepeatRepairDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  AltRepeat.repairCache(key_addr, key_state);
  return EventHandlerResult::OK;
}

EventHandlerResult AltRepeatDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  if (!keyToggledOn(key_state) || !key_addr.isValid()) {
    return EventHandlerResult::OK;
  }

  uint8_t offset = key_addr.toInt();

  if (offset >= MAX_TRACKED_KEYS) {
    return EventHandlerResult::OK;
  }

  if (key_state & INJECTED) {
    /* A Superkey hold or a timeline delivery replaced what this address holds. Track it so a re-press restores the current action */
    if (active_out_[offset] != Key_NoKey) {
      active_out_[offset] = mapped_key;
    }

    return EventHandlerResult::OK;
  }

  if (!keyRoleManager.isReplayingQueuedEvent()) {
    /* Repeat the repair for a press Combos flushes now, since its re-entry refreshed the live keymap again */
    repairCache(key_addr, key_state);
    return EventHandlerResult::OK;
  }

  Key output = findOutput(mapped_key, key_addr);
  active_out_[offset] = output;

  if (output == Key_NoKey) {
    return EventHandlerResult::OK;
  }

  mapped_key = output;

  /* Holds, the Qukeys restore loop and the release resolve through the live keymap */
  Layer.updateLiveCompositeKeymap(key_addr, output);
  return EventHandlerResult::OK;
}

bool AltRepeatDygma::configValid() const
{
  return p_config_ != nullptr && p_config_->layout_version == LAYOUT_VERSION && p_config_->rules_count <= MAX_RULES;
}

Key AltRepeatDygma::findOutput(Key mapped_key, KeyAddr key_addr) const
{
  if (!configValid() || p_config_->rules_count == 0) {
    return Key_NoKey;
  }

  uint16_t previous = AltRepeatTrack.decisionPrevious();

  if (previous == 0) {
    return Key_NoKey;
  }

  /* A combo's action replays at its anchor. It must not trigger the anchor's rules */
  if (Combos.ownsAddress(key_addr)) {
    return Key_NoKey;
  }

  if (!replaysCell(mapped_key, Layer.lookupOnActiveLayer(key_addr))) {
    return Key_NoKey;
  }

  uint8_t layer = Layer.lookupActiveLayer(key_addr);
  uint8_t position = key_addr.toInt();

  for (uint8_t i = 0; i < p_config_->rules_count; ++i) {
    const rule_t &rule = p_config_->rules[i];

    if (rule.layer == layer && rule.position == position && rule.previous == previous) {
      return Key(rule.output);
    }
  }

  return Key_NoKey;
}

/* Return true when the replayed key is the cell's own press, or the resolved tap of a dual-function cell */
bool AltRepeatDygma::replaysCell(Key mapped_key, Key cell)
{
  uint16_t raw = cell.getRaw();

  if ((raw >= ranges::AUTOSHIFT_FIRST && raw <= ranges::AUTOSHIFT_LAST) || raw == ranges::CAPS_WORD) {
    return false;
  }

  if (raw >= ranges::DYNAMIC_SUPER_FIRST && raw <= ranges::DYNAMIC_SUPER_LAST) {
    return false;
  }

  /* TapDance, Leader, Cycle, Syster, TopsyTurvy, Steno, SpaceCadet, Redial, Turbo and static macro codes. None of those plugins
   * runs here, and a Superkey holding Layer Shift 8 or 9 converts into this range */
  if (raw > ranges::DU_LAST && raw < ranges::DYNAMIC_MACRO_FIRST) {
    return false;
  }

  if (raw >= ranges::DU_FIRST && raw <= ranges::DU_LAST) {
    uint8_t keycode = mapped_key.getKeyCode();

    return (mapped_key.getFlags() & (SYNTHETIC | RESERVED)) == 0 && keycode != 0 &&
           !(keycode >= HID_KEYBOARD_FIRST_MODIFIER && keycode <= HID_KEYBOARD_LAST_MODIFIER);
  }

  return mapped_key == cell;
}

/* ------------------------------------------------------------------------- */
/* Rule values                                                               */
/* ------------------------------------------------------------------------- */

bool AltRepeatDygma::previousAllowed(uint16_t previous)
{
  uint8_t keycode = previous & 0xFF;
  uint8_t flags = previous >> 8;

  if (keycode < HID_KEYBOARD_A_AND_A || keycode > HID_LAST_KEY) {
    return false;
  }

  return (flags & (uint8_t)~MODS_MASK) == 0;
}

bool AltRepeatDygma::outputAllowed(uint16_t output)
{
  if (output == 0 || output == 0xFFFF) {
    return false;
  }

  if (output >= ranges::DU_FIRST && output <= ranges::DU_LAST) {
    return false;
  }

  if (output >= ranges::AUTOSHIFT_FIRST && output <= ranges::AUTOSHIFT_LAST) {
    return false;
  }

  /* A layer hold shortcut needs its layer key, which an output does not have */
  return (output & 0xE000) != 0x2000;
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult AltRepeatDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, "altrepeat.map")) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "altrepeat.map") != 0) {
    return EventHandlerResult::OK;
  }

  if (::Focus.isEOL()) {
    sendRules();
  } else if (readRules()) {
    cfgmem_config_save(&staging_);
  }

  /* A rejected write keeps the old table. FocusSerial drains the rest of the line */
  return EventHandlerResult::EVENT_CONSUMED;
}

void AltRepeatDygma::sendRules() const
{
  if (!configValid()) {
    ::Focus.send((uint8_t)0);
    return;
  }

  ::Focus.send(p_config_->rules_count);

  for (uint8_t i = 0; i < p_config_->rules_count; ++i) {
    const rule_t &rule = p_config_->rules[i];

    ::Focus.send(rule.layer, rule.position, rule.previous, rule.output);
  }
}

/* Wait for the next character as Stream::timedPeek does. Only a host write waits here */
int AltRepeatDygma::peekWait()
{
  uint32_t start = millis();

  do {
    int c = Runtime.serialPort().peek();

    if (c >= 0) {
      return c;
    }

    yield();
  } while (millis() - start < 1000);

  return -1;
}

/* Read one unsigned decimal of at most 0xFFFF. Fail at the end of the line, on anything but digits and on overflow */
bool AltRepeatDygma::readValue(uint16_t &value)
{
  int c = peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = peekWait();
  }

  if (c < '0' || c > '9') {
    return false;
  }

  uint32_t v = 0;

  while (c >= '0' && c <= '9') {
    v = v * 10 + (uint32_t)(c - '0');

    if (v > 0xFFFF) {
      return false;
    }

    Runtime.serialPort().read();
    c = peekWait();
  }

  value = (uint16_t)v;
  return true;
}

/* Parse a whole table into staging_. Return false on the first missing, malformed, out of range or duplicate value */
bool AltRepeatDygma::readRules()
{
  memset(&staging_, 0, sizeof(staging_));
  staging_.layout_version = LAYOUT_VERSION;

  uint16_t count = 0;

  if (!readValue(count) || count > MAX_RULES) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t layer = 0;
    uint16_t position = 0;
    uint16_t previous = 0;
    uint16_t output = 0;

    if (!readValue(layer) || !readValue(position) || !readValue(previous) || !readValue(output)) {
      return false;
    }

    if (layer >= ::layer_count || position >= Runtime.device().numKeys() || !previousAllowed(previous) || !outputAllowed(output)) {
      return false;
    }

    for (uint16_t j = 0; j < i; ++j) {
      const rule_t &staged = staging_.rules[j];

      if (staged.layer == layer && staged.position == position && staged.previous == previous) {
        return false;
      }
    }

    staging_.rules[i].layer = (uint8_t)layer;
    staging_.rules[i].position = (uint8_t)position;
    staging_.rules[i].previous = previous;
    staging_.rules[i].output = output;
  }

  int c = peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = peekWait();
  }

  if (c != '\n') {
    return false;
  }

  staging_.rules_count = (uint8_t)count;
  return true;
}

/* ------------------------------------------------------------------------- */
/* Setup and config memory                                                   */
/* ------------------------------------------------------------------------- */

EventHandlerResult AltRepeatDygma::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_ALTREPEAT, (const void **)&p_config_ );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  for (uint8_t i = 0; i < MAX_TRACKED_KEYS; ++i) {
    active_out_[i] = Key_NoKey;
  }

  /* Erased flash or another layout. Runs once after a flash */
  if (!configValid()) {
    memset(&staging_, 0, sizeof(staging_));
    staging_.layout_version = LAYOUT_VERSION;
    cfgmem_config_save(&staging_);
  }

  UNUSED( result );

  return EventHandlerResult::OK;
}

void AltRepeatDygma::cfgmem_config_save(const altrepeat_config_t *p_config)
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_data_save( p_config_, p_config, sizeof(altrepeat_config_t) );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

  UNUSED( result );
}

/* ------------------------------------------------------------------------- */
/* Tracker                                                                   */
/* ------------------------------------------------------------------------- */

static HIDReportObserver::SendReportHook chained_hook = nullptr;

/* Commit pending keys from every report first, then let the previously installed hook see it */
static void observeReport(uint8_t id, const void *data, int len, int result)
{
  AltRepeatTrack.commitReport(id, data, len);

  if (chained_hook != nullptr) {
    chained_hook(id, data, len, result);
  }
}

EventHandlerResult AltRepeatTrackDygma::onSetup()
{
  pending_len_ = 0;
  previous_ = 0;
  chained_hook = HIDReportObserver::resetHook(observeReport);
  return EventHandlerResult::OK;
}

EventHandlerResult AltRepeatTrackDygma::onKeyswitchEvent(Key &mapped_key, KeyAddr key_addr, uint8_t key_state)
{
  if (mapped_key.getFlags() & (SYNTHETIC | RESERVED)) {
    return EventHandlerResult::OK;
  }

  uint8_t keycode = mapped_key.getKeyCode();

  /* Only keys that type update history. Modifiers, layer, mouse, media and LED keys leave it unchanged */
  if (keycode < HID_KEYBOARD_A_AND_A || keycode > HID_LAST_KEY) {
    return EventHandlerResult::OK;
  }

  if (keyToggledOff(key_state)) {
    /* Only an injected release removes the key from the report being built (key_events.cpp). A physical or replayed release
     * leaves it in this cycle's report, so keep the entry */
    if (!(key_state & INJECTED)) {
      return EventHandlerResult::OK;
    }

    /* Released before any report carried it, so the host never saw it */
    uint8_t kept = 0;

    for (uint8_t i = 0; i < pending_len_; ++i) {
      if (pending_[i].keycode != keycode) {
        pending_[kept++] = pending_[i];
      }
    }

    pending_len_ = kept;
    return EventHandlerResult::OK;
  }

  if (!keyToggledOn(key_state)) {
    return EventHandlerResult::OK;
  }

  /* A Superkeys timeline duplicate is injected at the key's own address while the host already has the key */
  if ((key_state & INJECTED) && key_addr.isValid() && ::Keyboard.wasKeyPressed(keycode)) {
    return EventHandlerResult::OK;
  }

  for (uint8_t i = 0; i < pending_len_; ++i) {
    if (pending_[i].keycode == keycode) {
      return EventHandlerResult::OK;
    }
  }

  if (pending_len_ == FIFO_SIZE) {
    for (uint8_t i = 1; i < FIFO_SIZE; ++i) {
      pending_[i - 1] = pending_[i];
    }

    pending_len_--;
  }

  pending_[pending_len_].keycode = keycode;
  pending_[pending_len_].flags = foldFlags(mapped_key.getFlags());
  pending_len_++;
  return EventHandlerResult::OK;
}

EventHandlerResult AltRepeatTrackDygma::afterEachCycle()
{
  /* Drop keys that never reached a report this cycle, so a later report cannot commit them with the wrong modifiers */
  if (pending_len_ != 0) {
    pending_len_ = 0;
  }

  return EventHandlerResult::OK;
}

uint16_t AltRepeatTrackDygma::decisionPrevious() const
{
  if (pending_len_ == 0) {
    return previous_;
  }

  /* Not in a report yet. Take the key's own flags plus the modifiers pressed so far in the report being built */
  static const struct {
    Key key;
    uint8_t flag;
  } MODS[] = {
    {Key_LeftControl, CTRL_HELD}, {Key_RightControl, CTRL_HELD}, {Key_LeftShift, SHIFT_HELD}, {Key_RightShift, SHIFT_HELD},
    {Key_LeftAlt, LALT_HELD}, {Key_RightAlt, LALT_HELD}, {Key_LeftGui, GUI_HELD}, {Key_RightGui, GUI_HELD},
  };

  const Pending &newest = pending_[pending_len_ - 1];
  uint8_t flags = newest.flags;

  for (const auto &mod : MODS) {
    if (Runtime.hid().keyboard().isModifierKeyActive(mod.key)) {
      flags |= mod.flag;
    }
  }

  return (uint16_t)((foldFlags(flags) << 8) | newest.keycode);
}

void AltRepeatTrackDygma::commitReport(uint8_t id, const void *data, int len)
{
  if (pending_len_ == 0 || id != HID_REPORTID_NKRO_KEYBOARD || data == nullptr || len < 1) {
    return;
  }

  const uint8_t *report = static_cast<const uint8_t *>(data);

  /* Fold left and right modifiers of the report. AltGr counts as Alt */
  uint8_t sides = (uint8_t)((report[0] | (report[0] >> 4)) & 0x0F);
  uint8_t flags = 0;

  if (sides & 0x01) {
    flags |= CTRL_HELD;
  }

  if (sides & 0x02) {
    flags |= SHIFT_HELD;
  }

  if (sides & 0x04) {
    flags |= LALT_HELD;
  }

  if (sides & 0x08) {
    flags |= GUI_HELD;
  }

  uint8_t kept = 0;

  /* Commit in logical order, so the last key that reached the report is the previous key */
  for (uint8_t i = 0; i < pending_len_; ++i) {
    uint8_t keycode = pending_[i].keycode;
    int byte = 1 + keycode / 8;

    if (byte < len && (report[byte] & (1 << (keycode % 8)))) {
      previous_ = (uint16_t)((flags << 8) | keycode);
    } else {
      pending_[kept++] = pending_[i];
    }
  }

  pending_len_ = kept;
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::AltRepeatDygma AltRepeat;
kaleidoscope::plugin::AltRepeatRepairDygma AltRepeatRepair;
kaleidoscope::plugin::AltRepeatTrackDygma AltRepeatTrack;
