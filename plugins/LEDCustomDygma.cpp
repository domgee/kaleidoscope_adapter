/* -*- mode: c++ -*-
 * Kaleidoscope-LEDCustomDygma -- custom LED effect slots for Dygma keyboards
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

#include "LEDCustomDygma.h"
#include "kbdfal_ll_memory.h"

#include "AltRepeatDygma.h"
#include "Kaleidoscope-FocusSerial.h"

namespace kaleidoscope {
namespace plugin {

LEDCustomDygma::ledcustom_config_t LEDCustomDygma::staging_;

EventHandlerResult LEDCustomDygma::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_LEDCUSTOM, (const void **)&p_config_ );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Write an empty table over erased flash or another layout, which happens once after a flash */
  if (!configValid()) {
    memset(&staging_, 0, sizeof(staging_));
    staging_.layout_version = LAYOUT_VERSION;
    cfgmem_config_save(&staging_);
  }

  UNUSED( result );

  return EventHandlerResult::OK;
}

bool LEDCustomDygma::configValid() const
{
  return p_config_ != nullptr && p_config_->layout_version == LAYOUT_VERSION;
}

void LEDCustomDygma::setTableChangedHandler(table_changed_handler_t handler)
{
  table_changed_handler_ = handler;
}

bool LEDCustomDygma::slotGet(uint8_t slot, led_custom_t &settings) const
{
  if (!configValid() || slot >= MAX_SLOTS) {
    return false;
  }

  const slot_t &s = p_config_->slots[slot];

  if (s.type == LED_CUSTOM_TYPE_UNUSED || s.type > LED_CUSTOM_TYPE_LAST) {
    return false;
  }

  settings.type = s.type;
  settings.r = s.r;
  settings.g = s.g;
  settings.b = s.b;
  settings.speed = s.speed;

  return true;
}

/* Accept only the settings a type uses. Every other value must be 0 */
bool LEDCustomDygma::slotAllowed(const slot_t &s)
{
  switch (s.type) {
    case LED_CUSTOM_TYPE_SOLID:
      return s.speed == 0;

    case LED_CUSTOM_TYPE_RAINBOW:
      return s.r == 0 && s.g == 0 && s.b == 0 && (s.speed == 0 || s.speed >= LED_CUSTOM_RAINBOW_SPEED_MIN);

    case LED_CUSTOM_TYPE_UNUSED:
    case LED_CUSTOM_TYPE_RAINBOW_WAVE:
    case LED_CUSTOM_TYPE_STALKER:
    case LED_CUSTOM_TYPE_LAYERS:
    case LED_CUSTOM_TYPE_OFF:
      return s.r == 0 && s.g == 0 && s.b == 0 && s.speed == 0;

    default:
      return false;
  }
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult LEDCustomDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, "led.custom")) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "led.custom") != 0) {
    return EventHandlerResult::OK;
  }

  if (::Focus.isEOL()) {
    sendSlots();
  } else if (readSlots()) {
    bool changed = !configValid() || memcmp(p_config_, &staging_, sizeof(staging_)) != 0;

    cfgmem_config_save(&staging_);

    /* Let LEDManager leave a slot effect whose settings may have changed */
    if (changed && table_changed_handler_ != nullptr) {
      table_changed_handler_();
    }
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

/* Send the count, the highest used slot + 1, and then type r g b speed for each slot below it */
void LEDCustomDygma::sendSlots() const
{
  uint8_t count = 0;

  if (configValid()) {
    for (uint8_t i = 0; i < MAX_SLOTS; ++i) {
      if (p_config_->slots[i].type != LED_CUSTOM_TYPE_UNUSED) {
        count = i + 1;
      }
    }
  }

  ::Focus.send(count);

  for (uint8_t i = 0; i < count; ++i) {
    const slot_t &s = p_config_->slots[i];

    ::Focus.send(s.type, s.r, s.g, s.b, s.speed);
  }
}

/* Parse a whole table into staging_. Return false on the first missing, malformed or out of range value */
bool LEDCustomDygma::readSlots()
{
  memset(&staging_, 0, sizeof(staging_));
  staging_.layout_version = LAYOUT_VERSION;

  uint16_t count = 0;

  if (!AltRepeatDygma::readValue(count) || count > MAX_SLOTS) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t v[VALUES_PER_SLOT];

    for (uint8_t j = 0; j < VALUES_PER_SLOT; ++j) {
      if (!AltRepeatDygma::readValue(v[j]) || v[j] > 0xFF) {
        return false;
      }
    }

    slot_t &s = staging_.slots[i];

    s.type = (uint8_t)v[0];
    s.r = (uint8_t)v[1];
    s.g = (uint8_t)v[2];
    s.b = (uint8_t)v[3];
    s.speed = (uint8_t)v[4];

    if (!slotAllowed(s)) {
      return false;
    }
  }

  int c = AltRepeatDygma::peekWait();

  while (c == ' ') {
    Runtime.serialPort().read();
    c = AltRepeatDygma::peekWait();
  }

  return c == '\n';
}

void LEDCustomDygma::cfgmem_config_save(const ledcustom_config_t *p_config)
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_data_save( p_config_, p_config, sizeof(ledcustom_config_t) );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

  UNUSED( result );
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::LEDCustomDygma LEDCustom;
