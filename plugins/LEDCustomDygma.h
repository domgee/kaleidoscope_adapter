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

#pragma once

#include "kbd_core.h"

#include "kaleidoscope/Runtime.h"
#include "LEDEffect-Custom-Defy.h"

namespace kaleidoscope {
namespace plugin {

/* The table of custom LED effect slots and its Focus command led.custom. LEDControl looks a slot up when its key is pressed */
class LEDCustomDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t MAX_SLOTS       = LED_CUSTOM_SLOT_COUNT;
  static constexpr uint8_t LAYOUT_VERSION  = 1;
  static constexpr uint8_t VALUES_PER_SLOT = 5;

  typedef struct PACK
  {
      uint8_t type;           /* led_custom_type_t, 0 = unused                        */
      uint8_t r;
      uint8_t g;
      uint8_t b;
      uint8_t speed;          /* Rainbow delay_ms, 0 = built-in default, else 40..255 */
      uint8_t reserved[3];
  } slot_t;                   /* 8 B                                                  */

  typedef struct PACK
  {
      uint8_t layout_version; /* != LAYOUT_VERSION => wipe (0xFF erased) */
      uint8_t reserved[3];
      slot_t  slots[MAX_SLOTS];
  } ledcustom_config_t;       /* 132 B                                   */

  typedef void (*table_changed_handler_t)(void);

  EventHandlerResult onSetup();
  EventHandlerResult onFocusEvent(const char *command);

  /* Copy a used slot's settings. Return false for an unused slot, an unknown type or an invalid table */
  bool slotGet(uint8_t slot, led_custom_t &settings) const;

  /* Set the function called after a led.custom write changed the table */
  void setTableChangedHandler(table_changed_handler_t handler);

 private:
  const ledcustom_config_t *p_config_ = nullptr;
  table_changed_handler_t table_changed_handler_ = nullptr;

  /* A write is parsed here in full and saved only when every value is valid */
  static ledcustom_config_t staging_;

  bool configValid() const;
  static bool slotAllowed(const slot_t &s);
  bool readSlots();
  void sendSlots() const;
  void cfgmem_config_save(const ledcustom_config_t *p_config);
};

static_assert(sizeof(LEDCustomDygma::slot_t) == 8, "LEDCustom slot_t must stay 8 B");
static_assert(sizeof(LEDCustomDygma::ledcustom_config_t) == 132, "LEDCustom config block must stay 132 B");

}
}

extern kaleidoscope::plugin::LEDCustomDygma LEDCustom;
