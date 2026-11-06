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

#include "ConditionalLayersDygma.h"
#include "kbdfal_ll_memory.h"

#include "AltRepeatDygma.h"
#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/layers.h"

namespace kaleidoscope {
namespace plugin {

ConditionalLayersDygma::condlayers_config_t ConditionalLayersDygma::staging_;
uint8_t ConditionalLayersDygma::stack_pos_[MAX_LAYERS];

/* ------------------------------------------------------------------------- */
/* Setup and validity                                                        */
/* ------------------------------------------------------------------------- */

EventHandlerResult ConditionalLayersDygma::onSetup()
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_CONDLAYERS, (const void **)&p_config_ );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Write an empty table over erased flash, another layout or a rule the firmware does not accept */
  if (!configValid()) {
    memset(&staging_, 0, sizeof(staging_));
    staging_.layout_version = LAYOUT_VERSION;
    cfgmem_config_save(&staging_);
  }

  count_ = configValid() ? p_config_->count : 0;

  UNUSED( result );

  return EventHandlerResult::OK;
}

bool ConditionalLayersDygma::configValid() const
{
  return p_config_ != nullptr && tableAllowed(*p_config_);
}

bool ConditionalLayersDygma::ruleAllowed(const rule_t &r)
{
  uint16_t if_mask = r.if_mask;

  if ((if_mask & (uint16_t)~LAYERS_MASK) != 0 || __builtin_popcount(if_mask) < 2) {
    return false;
  }

  if (r.then_layer >= MAX_LAYERS) {
    return false;
  }

  return (if_mask & (uint16_t)(1u << r.then_layer)) == 0;
}

/* Check the header, every rule and that no rule repeats an earlier one exactly.
 * Load and write share it, so storage only holds what a write accepts */
bool ConditionalLayersDygma::tableAllowed(const condlayers_config_t &config)
{
  if (config.layout_version != LAYOUT_VERSION || config.count > MAX_RULES) {
    return false;
  }

  for (uint8_t i = 0; i < config.count; ++i) {
    if (!ruleAllowed(config.rules[i])) {
      return false;
    }

    for (uint8_t j = 0; j < i; ++j) {
      if (config.rules[j].if_mask == config.rules[i].if_mask && config.rules[j].then_layer == config.rules[i].then_layer) {
        return false;
      }
    }
  }

  return true;
}

/* ------------------------------------------------------------------------- */
/* Rules                                                                     */
/* ------------------------------------------------------------------------- */

/* Add in rounds the then-layer of every rule whose if-layers are all in the set the round started with, until a round adds nothing.
 * Record the then-layers in the order they are found, table order within a round */
uint16_t ConditionalLayersDygma::closure(uint16_t mask, uint8_t *order, uint8_t &order_len) const
{
  order_len = 0;

  if (count_ == 0 || p_config_ == nullptr) {
    return mask;
  }

  /* Each productive round adds a then-layer and there are at most MAX_RULES of them */
  for (uint8_t round = 0; round < MAX_RULES; ++round) {
    uint16_t next = mask;

    for (uint8_t i = 0; i < count_; ++i) {
      uint16_t if_mask = p_config_->rules[i].if_mask;
      uint8_t then_layer = p_config_->rules[i].then_layer;
      uint16_t bit = (uint16_t)(1u << then_layer);

      if ((next & bit) == 0 && (mask & if_mask) == if_mask) {
        next |= bit;

        if (order != nullptr) {
          order[order_len++] = then_layer;
        }
      }
    }

    if (next == mask) {
      break;
    }

    mask = next;
  }

  return mask;
}

uint16_t ConditionalLayersDygma::derived(uint16_t mask) const
{
  uint8_t order_len = 0;
  return closure(mask & LAYERS_MASK, nullptr, order_len);
}

uint16_t ConditionalLayersDygma::activeMask()
{
  uint16_t mask = 0;

  for (uint8_t layer = 0; layer < MAX_LAYERS; ++layer) {
    if (Layer.isActive(layer)) {
      mask |= (uint16_t)(1u << layer);
    }
  }

  return mask;
}

EventHandlerResult ConditionalLayersDygma::onLayerChange()
{
  evaluate();
  return EventHandlerResult::OK;
}

void ConditionalLayersDygma::evaluate()
{
  /* Ignore the layer changes the plugin makes itself. Check before any bookkeeping, so intermediate states never clear ownership */
  if (evaluating_) {
    return;
  }

  if (count_ == 0 && owned_ == 0) {
    return;
  }

  /* One pass reaches a stable state. The second pass is only a safety net */
  for (uint8_t pass = 0; pass < 2; ++pass) {
    uint16_t state = activeMask();

    /* Forget ownership after a Move, which leaves one layer at most. An owned layer otherwise has two active if-layers beside it */
    if (__builtin_popcount(state) <= 1) {
      owned_ = 0;
    }

    owned_ &= state;

    /* Derive from the layers other paths turned on, so a rule cycle can not keep itself on */
    uint8_t order[MAX_RULES];
    uint8_t order_len = 0;
    uint16_t desired = closure(state & (uint16_t)~owned_, order, order_len);
    uint16_t drop = owned_ & (uint16_t)~desired;
    uint16_t add = desired & (uint16_t)~state;

    if (drop == 0 && add == 0) {
      return;
    }

    evaluating_ = true;
    dropOwned(drop);

    for (uint8_t k = 0; k < order_len; ++k) {
      uint8_t layer = order[k];
      uint16_t bit = (uint16_t)(1u << layer);

      if ((add & bit) == 0) {
        continue;
      }

      Layer.activate(layer);

      if (Layer.isActive(layer)) {
        owned_ |= bit;
      }
    }

    evaluating_ = false;
  }
}

/* Turn off the owned layers no rule derives any more, top of the stack first, so the most recent layer changes as rarely as possible */
void ConditionalLayersDygma::dropOwned(uint16_t drop)
{
  memset(stack_pos_, 0, sizeof(stack_pos_));
  Layer.forEachActiveLayer(&recordStackPos);

  while (drop != 0) {
    uint8_t top = MAX_LAYERS;

    for (uint8_t layer = 0; layer < MAX_LAYERS; ++layer) {
      if ((drop & (1u << layer)) != 0 && (top == MAX_LAYERS || stack_pos_[layer] > stack_pos_[top])) {
        top = layer;
      }
    }

    drop &= (uint16_t)~(1u << top);
    owned_ &= (uint16_t)~(1u << top);
    Layer.deactivate(top);
  }
}

void ConditionalLayersDygma::recordStackPos(uint8_t index, uint8_t layer)
{
  if (layer < MAX_LAYERS) {
    stack_pos_[layer] = index;
  }
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult ConditionalLayersDygma::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, "conditional.map")) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command, "conditional.map") != 0) {
    return EventHandlerResult::OK;
  }

  if (::Focus.isEOL()) {
    sendRules();
  } else if (readRules()) {
    cfgmem_config_save(&staging_);
    count_ = configValid() ? p_config_->count : 0;
    /* Apply the new table at once, so owned then-layers that no rule derives any more turn off */
    evaluate();
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

void ConditionalLayersDygma::sendRules() const
{
  if (!configValid()) {
    ::Focus.send((uint8_t)0);
    return;
  }

  ::Focus.send(p_config_->count);

  for (uint8_t i = 0; i < p_config_->count; ++i) {
    /* Copy the packed members, since a reference to them may be misaligned */
    uint16_t if_mask = p_config_->rules[i].if_mask;
    uint8_t then_layer = p_config_->rules[i].then_layer;

    ::Focus.send(if_mask, then_layer);
  }
}

/* Parse a whole table into staging_. Return false on the first missing, malformed or out of range value, or a table a load would reject */
bool ConditionalLayersDygma::readRules()
{
  memset(&staging_, 0, sizeof(staging_));
  staging_.layout_version = LAYOUT_VERSION;

  uint16_t count = 0;

  if (!AltRepeatDygma::readValue(count) || count > MAX_RULES) {
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    uint16_t if_mask = 0;
    uint16_t then_layer = 0;

    if (!AltRepeatDygma::readValue(if_mask) || !AltRepeatDygma::readValue(then_layer) || then_layer > 0xFF) {
      return false;
    }

    staging_.rules[i].if_mask = if_mask;
    staging_.rules[i].then_layer = (uint8_t)then_layer;
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
  return tableAllowed(staging_);
}

void ConditionalLayersDygma::cfgmem_config_save(const condlayers_config_t *p_config)
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_data_save( p_config_, p_config, sizeof(condlayers_config_t) );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

  UNUSED( result );
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::ConditionalLayersDygma ConditionalLayers;
