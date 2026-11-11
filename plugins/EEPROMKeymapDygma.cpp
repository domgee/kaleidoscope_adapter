/* -*- mode: c++ -*-
 * Kaleidoscope-EEPROM-Keymap -- EEPROM-based keymap support.
 * Copyright (C) 2017, 2018, 2019  Keyboard.io, Inc
 * Copyright (C) 2025 Dygma Lab S.L. www.dygma.com
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

#include "EEPROMKeymapDygma.h"
#include "kbdfal_ll_memory.h"

#include <Kaleidoscope-FocusSerial.h>
#include "kaleidoscope/layers.h"

namespace kaleidoscope {
namespace plugin {
const EEPROMKeymap::keymap_config_t * EEPROMKeymap::p_keymap_config = nullptr;
uint8_t EEPROMKeymap::max_layers_;
uint8_t EEPROMKeymap::progmem_layers_;

EventHandlerResult EEPROMKeymap::onSetup() {
  result_t result = RESULT_ERR;

  progmem_layers_ = layer_count;        /* Save the number of default layers resolved at the compile time and stored in layer_count at the program start */

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_KEYMAP, (const void **)&p_keymap_config );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  /* Check if the configuration is valid */
  if( ( (uint8_t)p_keymap_config->ignore_hardcoded_layers ) == 0xFF )
  {
      cfgmem_ignore_hardcoded_layers_save( false );
  }

  max_layers_ = APP_LAYERS_CNT;
  layer_count = APP_LAYERS_CNT;
  if ( p_keymap_config->ignore_hardcoded_layers == true ) {
    Layer.getKey = getKey;
  } else {
    layer_count += progmem_layers_;
    Layer.getKey = getKeyExtended;
  }

  return EventHandlerResult::OK;

  UNUSED( result );
}

Key EEPROMKeymap::getKey(uint8_t layer, KeyAddr key_addr) {
  if (layer >= max_layers_)
    return Key_NoKey;

  uint16_t pos = (layer * Runtime.device().numKeys()) + key_addr.toInt();

  return Key( p_keymap_config->keys[pos].keyCode, p_keymap_config->keys[pos].flags );
}

Key EEPROMKeymap::getKeyExtended(uint8_t layer, KeyAddr key_addr) {

  // If the layer is within PROGMEM bounds, look it up from there
  if (layer < progmem_layers_) {
    return Layer.getKeyFromPROGMEM(layer, key_addr);
  }

  // If the layer is outside of PROGMEM, look up from EEPROM
  return getKey(layer - progmem_layers_, key_addr);
}

const EEPROMKeymap::keymap_config_t * EEPROMKeymap::getKeymapConfig(void) {
  return p_keymap_config;
}

void EEPROMKeymap::updateKey(uint16_t base_pos, Key key) {
  key_config_t key_config;

  key_config.flags = key.getFlags();
  key_config.keyCode = key.getKeyCode();

  cfgmem_key_save( &p_keymap_config->keys[ base_pos ], &key_config );
}

void EEPROMKeymap::dumpKeymap(uint8_t layers, Key(*getkey)(uint8_t, KeyAddr)) {
  for (uint8_t layer = 0; layer < layers; layer++) {
    for (auto key_addr : KeyAddr::all()) {
      Key k = (*getkey)(layer, key_addr);

      ::Focus.send(k);
    }
  }
}

EventHandlerResult EEPROMKeymap::onFocusEvent(const char *command) {
  if (::Focus.handleHelp(command, PSTR("keymap.custom\nkeymap.default\nkeymap.onlyCustom")))
    return EventHandlerResult::OK;

  if (strncmp_P(command, PSTR("keymap."), 7) != 0)
    return EventHandlerResult::OK;

  if (strcmp_P(command + 7, PSTR("onlyCustom")) == 0) {
    if (::Focus.isEOL()) {
      ::Focus.send((uint8_t)p_keymap_config->ignore_hardcoded_layers);
    } else {
      bool_t v;

      ::Focus.read((uint8_t &)v);

      cfgmem_ignore_hardcoded_layers_save( v );

      layer_count = max_layers_;
      if (v) {
        Layer.getKey = getKey;
      } else {
        layer_count += progmem_layers_;
        Layer.getKey = getKeyExtended;
      }
    }
    return EventHandlerResult::EVENT_CONSUMED;
  }

  if (strcmp_P(command + 7, PSTR("default")) == 0) {
    // By using a cast to the appropriate function type,
    // tell the compiler which overload of getKeyFromPROGMEM
    // we actully want.
    //
    dumpKeymap(progmem_layers_,
               static_cast<Key(*)(uint8_t, KeyAddr)>(Layer_::getKeyFromPROGMEM));
    return EventHandlerResult::EVENT_CONSUMED;
  }

  if (strcmp_P(command + 7, PSTR("custom")) != 0)
    return EventHandlerResult::OK;

  if (::Focus.isEOL()) {
    // By using a cast to the appropriate function type,
    // tell the compiler which overload of getKey
    // we actually want.
    //
    dumpKeymap(max_layers_, static_cast<Key(*)(uint8_t, KeyAddr)>(getKey));
  } else {
    uint16_t i = 0;

    while (!::Focus.isEOL() && (i < (uint16_t)Runtime.device().numKeys() * max_layers_)) {
      Key k;

      ::Focus.read(k);

      updateKey(i, k);
      i++;
    }
  }

  return EventHandlerResult::EVENT_CONSUMED;
}

/****************************************************/
/*                   Config Memory                  */
/****************************************************/

void EEPROMKeymap::cfgmem_ignore_hardcoded_layers_save( bool_t ignore_hardcoded_layers )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( &p_keymap_config->ignore_hardcoded_layers, &ignore_hardcoded_layers, sizeof( p_keymap_config->ignore_hardcoded_layers ) );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

    UNUSED( result );
}

void EEPROMKeymap::cfgmem_key_save( const key_config_t * p_key_config, key_config_t * p_key )
{
    result_t result = RESULT_ERR;

    result = kbdfal_ll_memory_data_save( p_key_config, p_key, sizeof( key_config_t ) );
    ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_data_save failed" );

    UNUSED( result );
}

}
}

kaleidoscope::plugin::EEPROMKeymap EEPROMKeymap;
