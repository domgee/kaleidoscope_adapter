/* UnicodeHexInput - Type Unicode scalars through macOS Unicode Hex Input.
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

#include <Arduino.h>

#include "kaleidoscope/key_defs.h"

namespace kaleidoscope {
namespace plugin {
namespace unicode_hex {

/* Time between two key events of a scalar */
static constexpr uint8_t  PACE_MS = 10;
static constexpr uint32_t INVALID = 0xFFFFFFFF;

/* Return the bytes of the UTF-8 scalar at p, a lead C2..F4 plus the continuation bytes 80..BF after it, at most the lead's count
 * and at most avail. Return 0 when p[0] is no lead */
uint8_t utf8Length(const uint8_t *p, uint16_t avail);

/* Decode len bytes from utf8Length. Return INVALID for truncated, overlong, surrogate or out of range sequences */
uint32_t utf8Decode(const uint8_t *p, uint8_t len);

/* Hold Left Alt for the key events of one scalar. Tap four hex digits per UTF-16 unit, then release Left Alt */
class Sequence {
 public:
  /* Start a scalar. Code points above U+FFFF become a surrogate pair */
  void begin(uint32_t cp);

  /* Return true while events are left */
  bool active() const;

  /* Return the next event. Return false when none is left */
  bool next(Key &key, bool &press);

  void reset();

 private:
  uint16_t units_[2] = {0, 0};
  uint8_t  count_    = 0;
  uint8_t  index_    = 0;

  uint8_t total() const;
};

}
}
}
