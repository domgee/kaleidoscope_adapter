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

#include "UnicodeHexInput.h"

namespace kaleidoscope {
namespace plugin {
namespace unicode_hex {

/* Bytes a UTF-8 lead announces, 0 for no lead */
static uint8_t leadLength(uint8_t lead)
{
  if (lead >= 0xC2 && lead <= 0xDF) {
    return 2;
  }

  if (lead >= 0xE0 && lead <= 0xEF) {
    return 3;
  }

  if (lead >= 0xF0 && lead <= 0xF4) {
    return 4;
  }

  return 0;
}

static bool isContinuation(uint8_t b)
{
  return b >= 0x80 && b <= 0xBF;
}

uint8_t utf8Length(const uint8_t *p, uint16_t avail)
{
  if (avail == 0) {
    return 0;
  }

  uint8_t need = leadLength(p[0]);

  if (need == 0) {
    return 0;
  }

  uint8_t n = 1;

  while (n < need && n < avail && isContinuation(p[n])) {
    n++;
  }

  return n;
}

uint32_t utf8Decode(const uint8_t *p, uint8_t len)
{
  if (len == 0 || leadLength(p[0]) != len) {
    return INVALID;
  }

  uint8_t lead = p[0];

  for (uint8_t n = 1; n < len; ++n) {
    if (!isContinuation(p[n])) {
      return INVALID;
    }
  }

  /* Reject overlong forms, surrogates and code points above U+10FFFF */
  if ((lead == 0xE0 && p[1] < 0xA0) || (lead == 0xED && p[1] >= 0xA0) || (lead == 0xF0 && p[1] < 0x90) || (lead == 0xF4 && p[1] >= 0x90)) {
    return INVALID;
  }

  uint32_t cp = (len == 2) ? (lead & 0x1F) : (len == 3) ? (lead & 0x0F) : (lead & 0x07);

  for (uint8_t n = 1; n < len; ++n) {
    cp = (cp << 6) | (p[n] & 0x3F);
  }

  return cp;
}

void Sequence::begin(uint32_t cp)
{
  if (cp >= 0x10000) {
    uint32_t v = cp - 0x10000;

    units_[0] = (uint16_t)(0xD800 | (v >> 10));
    units_[1] = (uint16_t)(0xDC00 | (v & 0x3FF));
    count_ = 2;
  } else {
    units_[0] = (uint16_t)cp;
    count_ = 1;
  }

  index_ = 0;
}

/* Left Alt down, a press and a release per hex digit, Left Alt up */
uint8_t Sequence::total() const
{
  return (count_ == 0) ? 0 : (uint8_t)(2 + count_ * 8);
}

bool Sequence::active() const
{
  return index_ < total();
}

bool Sequence::next(Key &key, bool &press)
{
  uint8_t last = total();

  if (index_ >= last) {
    return false;
  }

  uint8_t e = index_++;

  if (e == 0 || e == last - 1) {
    key = Key_LeftAlt;
    press = (e == 0);
    return true;
  }

  uint8_t d = (uint8_t)(e - 1);
  uint8_t nibble = (uint8_t)((units_[d / 8] >> (12 - 4 * ((d % 8) / 2))) & 0x0F);

  /* Top row digits and the letters A to F, with no Shift */
  uint8_t code = (nibble == 0) ? 0x27 : (nibble <= 9) ? (uint8_t)(0x1E + nibble - 1) : (uint8_t)(0x04 + nibble - 10);

  key = Key(code, KEY_FLAGS);
  press = (d % 2 == 0);
  return true;
}

void Sequence::reset()
{
  count_ = 0;
  index_ = 0;
}

}
}
}
