/* -*- mode: c++ -*-
 * Kaleidoscope-ConditionalLayersDygma -- ZMK-style conditional layers for Dygma keyboards
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

namespace kaleidoscope {
namespace plugin {

/* Conditional layers, as ZMK conditional layers and QMK tri-layer. Turn a rule's then-layer on while all its if-layers are active.
 * Turn a then-layer off only when this plugin turned it on */
class ConditionalLayersDygma : public kaleidoscope::Plugin {
 public:
  static constexpr uint8_t  MAX_RULES      = 8;
  static constexpr uint8_t  LAYOUT_VERSION = 1;
  static constexpr uint8_t  MAX_LAYERS     = 10;
  static constexpr uint16_t LAYERS_MASK    = 0x03FF;

  typedef struct PACK
  {
      uint16_t if_mask;      /* bit n = layer n, within LAYERS_MASK, 2 or more bits */
      uint8_t  then_layer;   /* < MAX_LAYERS, not in if_mask                       */
  } rule_t;                  /* 3 B                                                 */

  typedef struct PACK
  {
      uint8_t count;            /* <= MAX_RULES                            */
      uint8_t layout_version;   /* != LAYOUT_VERSION => wipe (0xFF erased) */
      rule_t  rules[MAX_RULES];
  } condlayers_config_t;        /* 26 B                                    */

  EventHandlerResult onSetup();
  EventHandlerResult onLayerChange();
  EventHandlerResult onFocusEvent(const char *command);

  /* Return mask plus every layer the rules derive from it. Ownership plays no part. LayerHoldMods uses it to keep a derived layer's mods */
  uint16_t derived(uint16_t mask) const;

  /* Return the active layers among 0..MAX_LAYERS-1 as a mask */
  static uint16_t activeMask();

  static bool ruleAllowed(const rule_t &r);
  static bool tableAllowed(const condlayers_config_t &config);

 private:
  const condlayers_config_t *p_config_ = nullptr;
  uint8_t  count_      = 0;
  uint16_t owned_      = 0;       /* then-layers this plugin turned on that are still on */
  bool     evaluating_ = false;   /* set while the plugin changes layers itself */

  /* A write is parsed here in full and saved only when the whole table is valid */
  static condlayers_config_t staging_;
  static uint8_t stack_pos_[MAX_LAYERS];

  bool configValid() const;
  uint16_t closure(uint16_t mask, uint8_t *order, uint8_t &order_len) const;
  void evaluate();
  void dropOwned(uint16_t drop);
  static void recordStackPos(uint8_t index, uint8_t layer);
  bool readRules();
  void sendRules() const;
  void cfgmem_config_save(const condlayers_config_t *p_config);
};

static_assert(sizeof(ConditionalLayersDygma::rule_t) == 3, "ConditionalLayers rule_t must stay 3 B");
static_assert(sizeof(ConditionalLayersDygma::condlayers_config_t) == 26, "ConditionalLayers config block must stay 26 B");

}
}

extern kaleidoscope::plugin::ConditionalLayersDygma ConditionalLayers;
