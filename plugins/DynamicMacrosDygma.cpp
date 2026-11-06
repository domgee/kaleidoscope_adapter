/* DynamicMacros - Dynamic macro support for Kaleidoscope.
 * Copyright (C) 2019  Keyboard.io, Inc.
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

#include "DynamicMacrosDygma.h"
#include "kbdfal_ll_memory.h"

#include "BaseDygma.h"
#include "hidDefy.h"
#include "OneShotDygma.h"
#include "QukeysDygma.h"

#include "Kaleidoscope-FocusSerial.h"
#include "kaleidoscope/key_events.h"
#include "kaleidoscope/keyswitch_state.h"

namespace kaleidoscope {
namespace plugin {

static_assert(MACRO_ACTION_STEP_TAP_CODE_SEQUENCE == 13, "New macro steps follow TAP_CODE_SEQUENCE");
static_assert(DynamicMacros::MAX_MACROS == ranges::DYNAMIC_MACRO_LAST - ranges::DYNAMIC_MACRO_FIRST + 1, "One map entry per macro key");

const DynamicMacros::macros_config_t * DynamicMacros::p_macros_config = nullptr;

uint16_t DynamicMacros::map_[];
uint8_t DynamicMacros::count_ = 0;

static constexpr uint16_t MEM = sizeof(DynamicMacros::macros_config_t::memory);

/* Modifier flags a Key carries */
static constexpr uint8_t FLAG_MODS = CTRL_HELD | LALT_HELD | RALT_HELD | SHIFT_HELD | GUI_HELD;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static bool isMacroKey(Key key)
{
  return key.getRaw() >= ranges::DYNAMIC_MACRO_FIRST && key.getRaw() <= ranges::DYNAMIC_MACRO_LAST;
}

static bool isKeyboardKey(Key key)
{
  return !(key.getFlags() & (SYNTHETIC | RESERVED));
}

static bool isConsumerKey(Key key)
{
  return (key.getFlags() & (SYNTHETIC | RESERVED | IS_CONSUMER)) == (SYNTHETIC | IS_CONSUMER);
}

static bool isModifierKeycode(Key key)
{
  return key.getKeyCode() >= HID_KEYBOARD_FIRST_MODIFIER && key.getKeyCode() <= HID_KEYBOARD_LAST_MODIFIER;
}

/* Payload bytes of the steps with a fixed size */
static uint8_t fixedLength(uint8_t type)
{
  switch (type) {
  case MACRO_ACTION_STEP_INTERVAL:
    return 4;

  case MACRO_ACTION_STEP_WAIT:
  case MACRO_ACTION_STEP_KEYDOWN:
  case MACRO_ACTION_STEP_KEYUP:
  case MACRO_ACTION_STEP_TAP:
    return 2;

  case MACRO_ACTION_STEP_KEYCODEDOWN:
  case MACRO_ACTION_STEP_KEYCODEUP:
  case MACRO_ACTION_STEP_TAPCODE:
  case DynamicMacros::STEP_OPTIONS:
    return 1;

  default:
    return 0;
  }
}

/* Return the bytes of the UTF-8 scalar at p, a lead C2..F4 plus the continuation bytes 80..BF after it, at most avail. Return 0
 * when p[0] is no lead */
static uint8_t utf8Length(const uint8_t *p, uint16_t avail)
{
  if (avail == 0) {
    return 0;
  }

  uint8_t lead = p[0];
  uint8_t need = 0;

  if (lead >= 0xC2 && lead <= 0xDF) {
    need = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    need = 3;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    need = 4;
  } else {
    return 0;
  }

  uint8_t n = 1;

  while (n < need && n < avail && p[n] >= 0x80 && p[n] <= 0xBF) {
    n++;
  }

  return n;
}

/* Key flag modifiers the last report carried */
static uint8_t sentFlagMods()
{
  auto &keyboard = Runtime.hid().keyboard();
  uint8_t mods = 0;

  if (keyboard.wasModifierKeyActive(Key_LeftControl)) {
    mods |= CTRL_HELD;
  }

  if (keyboard.wasModifierKeyActive(Key_LeftAlt)) {
    mods |= LALT_HELD;
  }

  if (keyboard.wasModifierKeyActive(Key_RightAlt)) {
    mods |= RALT_HELD;
  }

  if (keyboard.wasModifierKeyActive(Key_LeftShift)) {
    mods |= SHIFT_HELD;
  }

  if (keyboard.wasModifierKeyActive(Key_LeftGui)) {
    mods |= GUI_HELD;
  }

  return mods;
}

/* Press Key flag modifiers into the report, as Kaleidoscope's pressModifiers does */
static void pressFlagMods(uint8_t mods)
{
  auto &keyboard = Runtime.hid().keyboard();

  if (mods & CTRL_HELD) {
    keyboard.pressRawKey(Key_LeftControl);
  }

  if (mods & LALT_HELD) {
    keyboard.pressRawKey(Key_LeftAlt);
  }

  if (mods & RALT_HELD) {
    keyboard.pressRawKey(Key_RightAlt);
  }

  if (mods & SHIFT_HELD) {
    keyboard.pressRawKey(Key_LeftShift);
  }

  if (mods & GUI_HELD) {
    keyboard.pressRawKey(Key_LeftGui);
  }
}

/* ------------------------------------------------------------------------- */
/* Macro memory                                                              */
/* ------------------------------------------------------------------------- */

/* Payload bytes of the step at pos - 1, clipped to the memory end */
uint16_t DynamicMacros::stepLength(uint16_t pos, uint8_t type)
{
  const uint8_t *mem = p_macros_config->memory;
  uint16_t avail = (pos < MEM) ? (uint16_t)(MEM - pos) : 0;
  uint16_t n = fixedLength(type);

  if (type == MACRO_ACTION_STEP_TAP_SEQUENCE) {
    /* Count pairs up to and including the (0, 0) terminator */
    n = 0;

    while (n + 2 <= avail) {
      bool end = mem[pos + n] == 0 && mem[pos + n + 1] == 0;
      n += 2;

      if (end) {
        return n;
      }
    }

    return avail;
  }

  if (type == MACRO_ACTION_STEP_TAP_CODE_SEQUENCE) {
    /* Count bytes up to and including the 0 terminator */
    n = 0;

    while (n < avail) {
      if (mem[pos + n++] == 0) {
        return n;
      }
    }

    return avail;
  }

  if (type == STEP_UNICODE) {
    return utf8Length(&mem[pos], avail);
  }

  return (n > avail) ? avail : n;
}

void DynamicMacros::updateDynamicMacroCache(void)
{
  const uint8_t *mem = p_macros_config->memory;
  uint16_t pos = 0;
  uint8_t id = 0;
  bool previous_macro_ended = false;

  map_[0] = 0;
  count_ = 0;

  while (pos < MEM && id < MAX_MACROS) {
    uint8_t type = mem[pos++];

    if (type == MACRO_ACTION_END) {
      /* END END ends the map */
      if (previous_macro_ended) {
        break;
      }

      map_[++id] = pos;
      count_ = id;
      previous_macro_ended = true;
      continue;
    }

    previous_macro_ended = false;
    pos += stepLength(pos, type);
  }
}

/* Read the option step at the start of the macro. A macro without one plays once with the classic behaviour */
void DynamicMacros::optionsOf(uint8_t id, uint8_t &opts, uint16_t &body)
{
  const uint8_t *mem = p_macros_config->memory;
  uint16_t start = map_[id];

  opts = OPT_VALID;
  body = start;

  if (start + 1 >= MEM || mem[start] != STEP_OPTIONS) {
    return;
  }

  uint8_t o = mem[start + 1];

  if (!(o & OPT_VALID)) {
    o = OPT_VALID;
  }

  /* Mode 3 is reserved and plays once */
  if ((o & OPT_MODE) == OPT_MODE) {
    o &= (uint8_t)~OPT_MODE;
  }

  opts = o & OPT_KNOWN;
  body = start + 2;
}

/* ------------------------------------------------------------------------- */
/* Runs                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t DynamicMacros::findRun(uint8_t id) const
{
  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    if (runs_[i].state != RUN_FREE && runs_[i].id == id) {
      return i;
    }
  }

  return NONE;
}

uint8_t DynamicMacros::rootOf(uint8_t i) const
{
  for (uint8_t depth = 0; depth < MAX_RUNS && runs_[i].parent != NONE; ++depth) {
    i = runs_[i].parent;
  }

  return i;
}

uint8_t DynamicMacros::modeOf(const Run &r) const
{
  return (r.parent == NONE) ? (uint8_t)(r.opts & OPT_MODE) : MODE_ONCE;
}

KeyAddr DynamicMacros::injectingFor() const
{
  if (injecting_run_ == NONE) {
    return UnknownKeyswitchLocation;
  }

  return runs_[rootOf(injecting_run_)].trigger;
}

void DynamicMacros::wakeSoon()
{
  kbdtimer_set_ms(&timer_, 1);
  timer_pending_ = true;
}

void DynamicMacros::requestStop(uint8_t root)
{
  runs_[root].flags |= F_STOPPING;
  wakeSoon();
}

void DynamicMacros::startRoot(uint8_t id, KeyAddr key_addr)
{
  uint8_t i = 0;

  while (i < MAX_RUNS && runs_[i].state != RUN_FREE) {
    ++i;
  }

  if (i == MAX_RUNS) {
    last_error_ = ERR_POOL_FULL;
    return;
  }

  Run &r = runs_[i];

  r.id = id;
  r.parent = NONE;
  r.trigger = key_addr;
  optionsOf(id, r.opts, r.body);
  r.pos = r.body;
  r.sub = SUB_NONE;
  r.state = RUN_PLAYING;
  r.due_ms = Runtime.millisAtCycleStart();
  r.flags = F_FRESH | (key_addr.isValid() ? 0 : F_RELEASED);
  r.xflags = ::OneShot.isActive() ? X_ONESHOT : 0;

  active_count_++;
  wakeSoon();
}

/* Start a nested macro that the parent waits for. Return false when the step is skipped */
bool DynamicMacros::startChild(uint8_t i, uint8_t id)
{
  if (id >= count_) {
    last_error_ = ERR_BAD_ID;
    return false;
  }

  for (uint8_t a = i;; a = runs_[a].parent) {
    if (runs_[a].id == id) {
      last_error_ = ERR_CYCLE;
      return false;
    }

    if (runs_[a].parent == NONE) {
      break;
    }
  }

  if (findRun(id) != NONE) {
    last_error_ = ERR_BUSY;
    return false;
  }

  uint8_t c = 0;

  while (c < MAX_RUNS && runs_[c].state != RUN_FREE) {
    ++c;
  }

  if (c == MAX_RUNS) {
    last_error_ = ERR_POOL_FULL;
    return false;
  }

  Run &child = runs_[c];

  child.id = id;
  child.parent = i;
  child.trigger = UnknownKeyswitchLocation;
  optionsOf(id, child.opts, child.body);
  child.pos = child.body;
  child.sub = SUB_NONE;
  child.state = RUN_PLAYING;
  child.due_ms = Runtime.millisAtCycleStart();
  child.flags = F_RELEASED;
  child.xflags = 0;

  runs_[i].state = RUN_CHILD;
  active_count_++;
  return true;
}

/* Free a run. No other place does */
void DynamicMacros::freeRun(uint8_t i)
{
  runs_[i].state = RUN_FREE;
  active_count_--;
}

/* ------------------------------------------------------------------------- */
/* Stop tokens                                                               */
/* ------------------------------------------------------------------------- */

void DynamicMacros::addToken(KeyAddr key_addr, const uint8_t *ids, uint8_t count)
{
  if (!key_addr.isValid() || count == 0) {
    return;
  }

  uint8_t t = 0;

  while (t < token_count_ && tokens_[t].addr != key_addr) {
    ++t;
  }

  if (t == token_count_) {
    /* Replace the oldest token when all are in use */
    if (token_count_ == MAX_RUNS) {
      dropToken(0);
      t = MAX_RUNS - 1;
    }

    tokens_[t].addr = key_addr;
    tokens_[t].count = 0;
    token_count_ = t + 1;
  }

  Token &token = tokens_[t];

  for (uint8_t k = 0; k < count; ++k) {
    bool known = false;

    for (uint8_t j = 0; j < token.count; ++j) {
      known = known || token.ids[j] == ids[k];
    }

    if (!known && token.count < MAX_RUNS) {
      token.ids[token.count++] = ids[k];
    }
  }
}

void DynamicMacros::dropToken(uint8_t t)
{
  for (uint8_t j = t; j + 1 < token_count_; ++j) {
    tokens_[j] = tokens_[j + 1];
  }

  token_count_--;
}

/* ------------------------------------------------------------------------- */
/* Injection and held keys                                                   */
/* ------------------------------------------------------------------------- */

uint8_t DynamicMacros::injectPress(uint8_t i, Key key, uint8_t kind)
{
  /* Ignore a key press of a key the run already holds */
  if (!(kind & HELD_TEMP)) {
    for (uint8_t h = 0; h < held_count_; ++h) {
      if (held_[h].owner == i && !(held_[h].kind & HELD_TEMP) && held_[h].key == key) {
        return INJ_ALREADY;
      }
    }
  }

  if (held_count_ == MAX_HELD) {
    return INJ_FAILED;
  }

  injecting_run_ = i;
  handleKeyswitchEvent(key, UnknownKeyswitchLocation, IS_PRESSED | INJECTED);
  injecting_run_ = NONE;

  held_[held_count_].key = key;
  held_[held_count_].owner = i;
  held_[held_count_].kind = kind;
  held_count_++;

  runs_[i].flags |= F_ITER_OUTPUT;

  /* Forget the kept flag modifiers, since a new key press resets Kaleidoscope's modifier flag mask */
  if (isKeyboardKey(key)) {
    flag_hold_ = 0;
  }

  return INJ_OK;
}

void DynamicMacros::removeHeld(uint8_t h)
{
  for (uint8_t j = h; j + 1 < held_count_; ++j) {
    held_[j] = held_[j + 1];
  }

  held_count_--;
}

void DynamicMacros::releaseTemp(uint8_t i, Key key)
{
  for (uint8_t h = held_count_; h-- > 0;) {
    if (held_[h].owner == i && (held_[h].kind & HELD_TEMP) && held_[h].key == key) {
      removeHeld(h);
      releaseEvent(i, key);
      return;
    }
  }
}

/* Release a key press step. Send the release also when the run never pressed the key, as before */
void DynamicMacros::releasePersistent(uint8_t i, Key key)
{
  uint8_t found = NONE;

  for (uint8_t h = held_count_; h-- > 0;) {
    if (held_[h].owner != i || held_[h].key != key) {
      continue;
    }

    if (!(held_[h].kind & HELD_TEMP)) {
      found = h;
      break;
    }

    if (found == NONE) {
      found = h;
    }
  }

  if (found != NONE) {
    removeHeld(found);
  }

  releaseEvent(i, key);
}

void DynamicMacros::releaseEvent(uint8_t i, Key key)
{
  injecting_run_ = i;
  handleKeyswitchEvent(key, UnknownKeyswitchLocation, WAS_PRESSED | INJECTED);
  injecting_run_ = NONE;

  /* Release a consumer usage explicitly, since its toggle-off leaves it in the report */
  if (isConsumerKey(key)) {
    for (uint8_t h = 0; h < held_count_; ++h) {
      if (held_[h].key == key) {
        return;
      }
    }

    Runtime.hid().keyboard().releaseConsumerControl(key);
    return;
  }

  if (!isKeyboardKey(key)) {
    return;
  }

  /* Press the keys the macros still hold again, since the release took their keycode or flag modifiers out of the report */
  for (uint8_t h = 0; h < held_count_; ++h) {
    if (isKeyboardKey(held_[h].key)) {
      Runtime.hid().keyboard().pressKey(held_[h].key, false);
    }
  }

  /* Keep the flag modifiers the release cleared from Kaleidoscope's mask, when the last report carried them */
  uint8_t mods = key.getFlags() & FLAG_MODS;

  if (mods != 0) {
    flag_hold_ |= mods & sentFlagMods();
  }
}

void DynamicMacros::releaseHeldOf(uint8_t i)
{
  for (uint8_t h = held_count_; h-- > 0;) {
    if (held_[h].owner == i) {
      Key key = held_[h].key;

      removeHeld(h);
      releaseEvent(i, key);
    }
  }
}

/* Flag modifiers of the non-modifier keys the macros hold */
uint8_t DynamicMacros::heldFlagMods() const
{
  uint8_t mods = 0;

  for (uint8_t h = 0; h < held_count_; ++h) {
    Key key = held_[h].key;

    if (isKeyboardKey(key) && !isModifierKeycode(key)) {
      mods |= key.getFlags() & FLAG_MODS;
    }
  }

  return mods;
}

/* Stop the tree when the held table is full. Nothing was acquired for the failed key */
void DynamicMacros::failStop(uint8_t i)
{
  runs_[i].sub = SUB_NONE;
  last_error_ = ERR_HELD_FULL;
  requestStop(rootOf(i));
}

/* ------------------------------------------------------------------------- */
/* Player                                                                    */
/* ------------------------------------------------------------------------- */

uint16_t DynamicMacros::randomDelay(uint16_t min_ms, uint16_t max_ms)
{
  if (min_ms > max_ms) {
    uint16_t swap = min_ms;
    min_ms = max_ms;
    max_ms = swap;
  }

  if (rng_ == 0) {
    rng_ = Runtime.millisAtCycleStart() | 1;
  }

  /* xorshift32 */
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;

  uint32_t range = (uint32_t)(max_ms - min_ms) + 1;
  return (uint16_t)(min_ms + rng_ % range);
}

/* Play steps of run i until one output unit went out. Return true when it did */
bool DynamicMacros::step(uint8_t i, uint32_t now)
{
  Run &r = runs_[i];

  /* Release the tap or sequence element the last unit pressed */
  if (r.sub == SUB_TAP_RELEASE || r.sub == SUB_SEQ_RELEASE || r.sub == SUB_CODESEQ_RELEASE) {
    releaseTemp(i, r.sub_key);
    r.flags |= F_ITER_OUTPUT;
    r.sub = (r.sub == SUB_TAP_RELEASE) ? (uint8_t)SUB_NONE : (uint8_t)(r.sub - 1);
    return true;
  }

  if (runs_[rootOf(i)].flags & F_STOPPING) {
    return false;
  }

  const uint8_t *mem = p_macros_config->memory;
  bool batch = false;

  for (uint8_t budget = STEPS_PER_CYCLE; budget > 0; --budget) {
    if (r.sub == SUB_SEQ || r.sub == SUB_CODESEQ) {
      uint8_t size = (r.sub == SUB_SEQ) ? 2 : 1;

      if (r.pos + size > MEM) {
        r.sub = SUB_NONE;
        r.pos = MEM;
        endOfMacro(i, now);
        return batch;
      }

      Key key = (r.sub == SUB_SEQ) ? Key(mem[r.pos + 1], mem[r.pos]) : Key(mem[r.pos], 0);

      if (key.getRaw() == 0) {
        r.pos += size;
        r.sub = SUB_NONE;
        continue;
      }

      if (isMacroKey(key)) {
        r.pos += size;
        continue;
      }

      if (injectPress(i, key, HELD_TEMP) == INJ_FAILED) {
        failStop(i);
        return batch;
      }

      r.pos += size;
      r.sub_key = key;
      r.sub = (uint8_t)(r.sub + 1);
      return true;
    }

    if (r.pos >= MEM) {
      endOfMacro(i, now);
      return batch;
    }

    uint8_t type = mem[r.pos];
    uint16_t p = r.pos + 1;

    /* A step cut off by the memory end ends the macro */
    if (p + fixedLength(type) > MEM) {
      r.pos = MEM;
      endOfMacro(i, now);
      return batch;
    }

    uint16_t len = stepLength(p, type);

    switch (type) {
    case MACRO_ACTION_END:
      r.pos = p;
      endOfMacro(i, now);
      return batch;

    case MACRO_ACTION_STEP_INTERVAL:
    case MACRO_ACTION_STEP_WAIT: {
      uint16_t ms = (uint16_t)((mem[p] << 8) | mem[p + 1]);

      if (type == MACRO_ACTION_STEP_INTERVAL) {
        ms = randomDelay(ms, (uint16_t)((mem[p + 2] << 8) | mem[p + 3]));
      }

      r.pos = p + len;

      if (ms == 0) {
        continue;
      }

      r.state = RUN_WAITING;
      r.due_ms = now + ms;
      r.flags |= F_ITER_OUTPUT;
      return batch;
    }

    case MACRO_ACTION_STEP_KEYDOWN:
    case MACRO_ACTION_STEP_KEYCODEDOWN: {
      Key key = (type == MACRO_ACTION_STEP_KEYDOWN) ? Key(mem[p + 1], mem[p]) : Key(mem[p], 0);
      r.pos = p + len;

      if (type == MACRO_ACTION_STEP_KEYDOWN && isMacroKey(key)) {
        if (startChild(i, (uint8_t)(key.getRaw() - ranges::DYNAMIC_MACRO_FIRST))) {
          return batch;
        }

        continue;
      }

      uint8_t result = injectPress(i, key, 0);

      if (result == INJ_FAILED) {
        r.pos = p - 1;
        failStop(i);
        return batch;
      }

      if (result == INJ_ALREADY) {
        continue;
      }

      if (r.flags & F_EXPLICIT) {
        batch = true;
        continue;
      }

      return true;
    }

    case MACRO_ACTION_STEP_KEYUP:
    case MACRO_ACTION_STEP_KEYCODEUP: {
      Key key = (type == MACRO_ACTION_STEP_KEYUP) ? Key(mem[p + 1], mem[p]) : Key(mem[p], 0);
      r.pos = p + len;

      if (type == MACRO_ACTION_STEP_KEYUP && isMacroKey(key)) {
        continue;
      }

      releasePersistent(i, key);
      r.flags |= F_ITER_OUTPUT;

      if (r.flags & F_EXPLICIT) {
        batch = true;
        continue;
      }

      return true;
    }

    case MACRO_ACTION_STEP_TAP:
    case MACRO_ACTION_STEP_TAPCODE: {
      Key key = (type == MACRO_ACTION_STEP_TAP) ? Key(mem[p + 1], mem[p]) : Key(mem[p], 0);

      if (type == MACRO_ACTION_STEP_TAP && isMacroKey(key)) {
        r.pos = p + len;

        if (startChild(i, (uint8_t)(key.getRaw() - ranges::DYNAMIC_MACRO_FIRST))) {
          return batch;
        }

        continue;
      }

      if (injectPress(i, key, HELD_TEMP) == INJ_FAILED) {
        failStop(i);
        return batch;
      }

      r.pos = p + len;
      r.sub_key = key;
      r.sub = SUB_TAP_RELEASE;
      return true;
    }

    case MACRO_ACTION_STEP_EXPLICIT_REPORT:
      r.flags |= F_EXPLICIT;
      r.pos = p;
      continue;

    case MACRO_ACTION_STEP_IMPLICIT_REPORT:
      r.flags &= (uint8_t)~F_EXPLICIT;
      r.pos = p;
      continue;

    case MACRO_ACTION_STEP_SEND_REPORT:
      r.pos = p;

      if (batch) {
        return true;
      }

      continue;

    case MACRO_ACTION_STEP_TAP_SEQUENCE:
    case MACRO_ACTION_STEP_TAP_CODE_SEQUENCE:
      r.pos = p;
      r.sub = (type == MACRO_ACTION_STEP_TAP_SEQUENCE) ? SUB_SEQ : SUB_CODESEQ;
      continue;

    case STEP_OPTIONS:
      r.pos = p + len;
      continue;

    case STEP_RELEASE:
      r.pos = p;

      /* Pause at the first marker of a root that plays once, until its key is released */
      if (r.parent == NONE && modeOf(r) == MODE_ONCE && !(r.flags & F_MARKER_SEEN)) {
        r.flags |= F_MARKER_SEEN;

        if (!(r.flags & F_RELEASED)) {
          r.state = RUN_PAUSED;
          return batch;
        }
      }

      continue;

    case STEP_UNICODE:
      /* Skip the scalar */
      r.pos = p + len;
      continue;

    default:
      r.pos = p;
      continue;
    }
  }

  /* Leave the run PLAYING when the budget is used up, so armTimer wakes it again */
  return batch;
}

void DynamicMacros::endOfMacro(uint8_t i, uint32_t now)
{
  Run &r = runs_[i];

  /* Hand the child's progress to the parent, which goes on after the child's step */
  if (r.parent != NONE) {
    uint8_t parent = r.parent;

    if (r.flags & F_ITER_OUTPUT) {
      runs_[parent].flags |= F_ITER_OUTPUT;
    }

    releaseHeldOf(i);
    freeRun(i);
    runs_[parent].state = RUN_PLAYING;
    return;
  }

  uint8_t mode = modeOf(r);

  if (mode == MODE_ONCE) {
    /* Play once more for a Superkey that sends its key twice */
    if (r.flags & F_REPLAY) {
      releaseHeldOf(i);
      r.flags &= (uint8_t)~(F_REPLAY | F_MARKER_SEEN | F_EXPLICIT);
      r.pos = r.body;
      r.state = RUN_WAITING;
      r.due_ms = now + 1;
      return;
    }

    finishRoot(i, true);
    return;
  }

  r.flags |= F_ITER_DONE;

  /* End a loop that does nothing, and a repeat whose key is already up */
  if (!(r.flags & F_ITER_OUTPUT) || (mode == MODE_REPEAT && (r.flags & F_RELEASED))) {
    finishRoot(i, true);
    return;
  }

  /* Yield once on the timer between iterations and keep the held keys */
  r.flags &= (uint8_t)~(F_ITER_OUTPUT | F_EXPLICIT);
  r.pos = r.body;
  r.state = RUN_WAITING;
  r.due_ms = now + 1;
}

/* Release and free run i and its descendants, deepest first */
void DynamicMacros::finishTree(uint8_t i)
{
  for (uint8_t j = 0; j < MAX_RUNS; ++j) {
    if (runs_[j].state != RUN_FREE && runs_[j].parent == i) {
      finishTree(j);
    }
  }

  releaseHeldOf(i);
  freeRun(i);
}

void DynamicMacros::finishRoot(uint8_t i, bool at_end)
{
  uint8_t xflags = runs_[i].xflags;

  finishTree(i);

  /* End the OneShot modifiers the macro used, as at the end of a macro before */
  if (at_end && (xflags & X_ONESHOT) && ::OneShot.isActive() && !::OneShot.isPressed()) {
    ::OneShot.cancel(true);
  }
}

/* Finish stopped trees. Their pending tap and sequence releases go out first */
void DynamicMacros::finishStops()
{
  for (uint8_t root = 0; root < MAX_RUNS; ++root) {
    Run &r = runs_[root];

    if (r.state == RUN_FREE || r.parent != NONE || !(r.flags & F_STOPPING)) {
      continue;
    }

    for (uint8_t j = 0; j < MAX_RUNS; ++j) {
      Run &run = runs_[j];

      if (run.state == RUN_FREE || rootOf(j) != root) {
        continue;
      }

      if (run.sub == SUB_TAP_RELEASE || run.sub == SUB_SEQ_RELEASE || run.sub == SUB_CODESEQ_RELEASE) {
        releaseTemp(j, run.sub_key);
        run.sub = SUB_NONE;
      }
    }

    finishRoot(root, true);
  }
}

/* Step the playing runs round-robin until one sends an output unit */
bool DynamicMacros::pickAndStep(uint32_t now)
{
  for (uint8_t n = 1; n <= MAX_RUNS; ++n) {
    uint8_t i = (uint8_t)((rr_ + n) % MAX_RUNS);

    if (runs_[i].state != RUN_PLAYING) {
      continue;
    }

    if (step(i, now)) {
      rr_ = i;
      return true;
    }
  }

  return false;
}

/* Wake in 1 ms while a run can play, at the nearest deadline while runs wait, and not at all while runs are paused */
void DynamicMacros::armTimer(uint32_t now)
{
  if (active_count_ == 0) {
    goIdle();
    return;
  }

  uint32_t ms = 0;

  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    if (runs_[i].state == RUN_PLAYING) {
      ms = 1;
      break;
    }
  }

  if (ms == 0) {
    for (uint8_t i = 0; i < MAX_RUNS; ++i) {
      if (runs_[i].state != RUN_WAITING) {
        continue;
      }

      int32_t left = (int32_t)(runs_[i].due_ms - now);
      uint32_t wait = (left < 1) ? 1 : (uint32_t)left;

      if (ms == 0 || wait < ms) {
        ms = wait;
      }
    }
  }

  if (ms == 0) {
    timer_pending_ = false;
    return;
  }

  kbdtimer_set_ms(&timer_, ms);
  timer_pending_ = true;
}

void DynamicMacros::goIdle()
{
  timer_pending_ = false;
  flag_hold_ = 0;
  rr_ = 0;
}

/* Stop every macro at once and release what they hold */
void DynamicMacros::abortAll(uint8_t err)
{
  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    if (runs_[i].state != RUN_FREE && runs_[i].parent == NONE) {
      finishRoot(i, false);
    }
  }

  if (err != ERR_NONE) {
    last_error_ = err;
  }

  goIdle();
}

/* ------------------------------------------------------------------------- */
/* Key events                                                                */
/* ------------------------------------------------------------------------- */

void DynamicMacros::onMacroPress(uint8_t id, KeyAddr key_addr, uint8_t key_state)
{
  if (id >= count_) {
    last_error_ = ERR_BAD_ID;
    return;
  }

  uint8_t i = findRun(id);

  if (i == NONE) {
    startRoot(id, key_addr);
    return;
  }

  Run &r = runs_[i];

  if (r.parent != NONE) {
    last_error_ = ERR_BUSY;
    return;
  }

  /* Replay once for a Superkey that sends its key twice, as press and release at the trigger in the start cycle */
  if ((r.flags & F_FRESH) && (key_state & INJECTED) && key_addr.isValid() && key_addr == r.trigger && (r.flags & F_RELEASED)) {
    if (modeOf(r) == MODE_ONCE) {
      r.flags |= F_REPLAY;
    }

    r.flags &= (uint8_t)~F_FRESH;
    return;
  }

  /* Stop the macro on its own key, also while it waits for the release. The release half never plays */
  requestStop(i);
  addToken(key_addr, &r.id, 1);
}

void DynamicMacros::onMacroRelease(uint8_t id, KeyAddr key_addr)
{
  uint8_t i = findRun(id);

  if (i == NONE || runs_[i].parent != NONE || !key_addr.isValid() || key_addr != runs_[i].trigger) {
    return;
  }

  onTriggerReleased(i);
}

void DynamicMacros::onTriggerReleased(uint8_t i)
{
  Run &r = runs_[i];

  if (r.flags & F_RELEASED) {
    return;
  }

  r.flags |= F_RELEASED;

  if (r.state == RUN_PAUSED) {
    r.state = RUN_PLAYING;
    wakeSoon();
  }

  /* Stop repeat while held at the release once it played a full iteration */
  if (modeOf(r) == MODE_REPEAT && (r.flags & F_ITER_DONE)) {
    requestStop(i);
  }
}

void DynamicMacros::notePhysicalPress(KeyAddr key_addr)
{
  /* Forget the kept flag modifiers, since a new key press resets Kaleidoscope's modifier flag mask */
  flag_hold_ = 0;

  /* End the token of an earlier press at this address */
  for (uint8_t t = 0; t < token_count_; ++t) {
    if (tokens_[t].addr == key_addr) {
      dropToken(t);
      break;
    }
  }

  if (active_count_ == 0) {
    return;
  }

  uint8_t ids[MAX_RUNS];
  uint8_t count = 0;

  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    Run &r = runs_[i];

    if (r.state != RUN_PLAYING && r.state != RUN_WAITING && r.state != RUN_CHILD) {
      continue;
    }

    if (r.parent != NONE || !(r.opts & OPT_OTHER_KEYS_STOP) || (r.flags & F_STOPPING) || key_addr == r.trigger) {
      continue;
    }

    requestStop(i);
    ids[count++] = r.id;
  }

  /* Let the press itself through */
  addToken(key_addr, ids, count);
}

EventHandlerResult DynamicMacros::onKeyswitchEvent(Key &mappedKey, KeyAddr key_addr, uint8_t keyState)
{
  /* End the token at a stopping address on its resolved press, and swallow that press when it would start a macro it stopped */
  if (token_count_ != 0 && keyToggledOn(keyState) && key_addr.isValid()) {
    for (uint8_t t = 0; t < token_count_; ++t) {
      if (tokens_[t].addr != key_addr) {
        continue;
      }

      bool swallow = false;

      if (isMacroKey(mappedKey)) {
        uint8_t id = (uint8_t)(mappedKey.getRaw() - ranges::DYNAMIC_MACRO_FIRST);

        for (uint8_t k = 0; k < tokens_[t].count; ++k) {
          swallow = swallow || tokens_[t].ids[k] == id;
        }
      }

      dropToken(t);

      if (swallow) {
        return EventHandlerResult::EVENT_CONSUMED;
      }

      break;
    }
  }

  if (!isMacroKey(mappedKey)) {
    return EventHandlerResult::OK;
  }

  uint8_t id = (uint8_t)(mappedKey.getRaw() - ranges::DYNAMIC_MACRO_FIRST);

  if (keyToggledOn(keyState)) {
    onMacroPress(id, key_addr, keyState);
  } else if (keyToggledOff(keyState)) {
    onMacroRelease(id, key_addr);
  }

  return EventHandlerResult::EVENT_CONSUMED;
}

/* ------------------------------------------------------------------------- */
/* Cycle hooks                                                               */
/* ------------------------------------------------------------------------- */

EventHandlerResult DynamicMacros::beforeEachCycle()
{
  if (active_count_ == 0) {
    return EventHandlerResult::OK;
  }

  /* Hold the keys the macros pressed in the new report */
  for (uint8_t h = 0; h < held_count_; ++h) {
    injecting_run_ = held_[h].owner;
    handleKeyswitchEvent(held_[h].key, UnknownKeyswitchLocation, IS_PRESSED | WAS_PRESSED | INJECTED);
  }

  injecting_run_ = NONE;
  return EventHandlerResult::OK;
}

EventHandlerResult DynamicMacros::beforeReportingState()
{
  if (active_count_ == 0) {
    return EventHandlerResult::OK;
  }

  if (timer_pending_ && kbdtimer_check(&timer_)) {
    timer_pending_ = false;
  }

  /* Stop everything while the keyboard API holds key reports off or no host takes them */
  if (!::BaseDygma.isKeyReportEnabled() || !::HID().outputReady()) {
    abortAll(ERR_OUTPUT);
    return EventHandlerResult::OK;
  }

  uint32_t now = Runtime.millisAtCycleStart();

  /* Treat a trigger that is no longer held as released when another plugin consumed its release */
  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    Run &r = runs_[i];

    if (r.state == RUN_FREE || r.parent != NONE || (r.flags & F_RELEASED) || !r.trigger.isValid()) {
      continue;
    }

    if (!Runtime.device().isKeyswitchPressed(r.trigger) && !::qukeys.hasQueuedReleaseFirst(r.trigger)) {
      onTriggerReleased(i);
    }
  }

  finishStops();

  bool playing = false;

  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    Run &r = runs_[i];

    if (r.state == RUN_WAITING && (int32_t)(now - r.due_ms) >= 0) {
      r.state = RUN_PLAYING;
    }

    playing = playing || r.state == RUN_PLAYING;
  }

  /* Wait while the HID FIFO holds about two reports, so output never overruns it */
  if (playing && ::HID().pendingReportBytes() < BACKPRESSURE_BYTES) {
    pickAndStep(now);
  }

  finishStops();

  if (flag_hold_ != 0) {
    flag_hold_ &= heldFlagMods();
    pressFlagMods(flag_hold_);
  }

  for (uint8_t i = 0; i < MAX_RUNS; ++i) {
    runs_[i].flags &= (uint8_t)~F_FRESH;
  }

  armTimer(now);
  return EventHandlerResult::OK;
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                     */
/* ------------------------------------------------------------------------- */

EventHandlerResult DynamicMacros::onFocusEvent(const char *command)
{
  if (::Focus.handleHelp(command, PSTR("macros.map\nmacros.trigger\nmacros.memory"))) {
    return EventHandlerResult::OK;
  }

  /* Release everything before a firmware update */
  if (strncmp_P(command, PSTR("upgrade."), 8) == 0) {
    abortAll(ERR_NONE);
    return EventHandlerResult::OK;
  }

  if (strncmp_P(command, PSTR("macros."), 7) != 0) {
    return EventHandlerResult::OK;
  }

  if (strcmp_P(command + 7, PSTR("map")) == 0) {
    if (::Focus.isEOL()) {
      for (uint16_t i = 0; i < sizeof(p_macros_config->memory); i++) {
        ::Focus.send(p_macros_config->memory[i]);
      }
    } else {
      /* Stop all macros before their steps change under them */
      abortAll(ERR_NONE);

      uint16_t pos = 0;

      while (!::Focus.isEOL()) {
        uint8_t b;
        ::Focus.read(b);

        cfgmem_macro_mem_save( pos++, &b, sizeof(b) );
      }

      updateDynamicMacroCache();
    }
  }

  if (strcmp_P(command + 7, PSTR("trigger")) == 0) {
    uint8_t id = 0;
    ::Focus.read(id);

    if (id >= count_) {
      last_error_ = ERR_BAD_ID;
    } else {
      uint8_t i = findRun(id);

      if (i == NONE) {
        startRoot(id, UnknownKeyswitchLocation);
      } else if (runs_[i].parent != NONE) {
        last_error_ = ERR_BUSY;
      } else {
        requestStop(i);
      }
    }
  }

  if (strcmp_P(command + 7, PSTR("memory")) == 0) {
    if (::Focus.isEOL()) {
      ::Focus.send(sizeof( p_macros_config->memory ));
    }
  }

  return EventHandlerResult::EVENT_CONSUMED;
}

void DynamicMacros::reserve_storage( void )
{
  result_t result = RESULT_ERR;

  result = kbdfal_ll_memory_item_request( KBDMEM_ITEM_TYPE_MACROS, (const void **)&p_macros_config );
  ASSERT_DYGMA( result == RESULT_OK, "kbdfal_ll_memory_item_request failed" );

  updateDynamicMacroCache();

  UNUSED( result );
}

/****************************************************/
/*                   Config Memory                  */
/****************************************************/

void DynamicMacros::cfgmem_macro_mem_save( uint16_t mem_pos, uint8_t * p_data, uint16_t data_len )
{
    result_t result = RESULT_ERR;

    if( ( mem_pos + data_len ) > sizeof( p_macros_config->memory ) )
    {
        ASSERT_DYGMA( false, "Macro memory space overflow" );
        return;
    }

    result = kbdfal_ll_memory_data_save( &p_macros_config->memory[mem_pos], p_data, data_len );
    ASSERT_DYGMA( result == RESULT_OK, "ConfigManager.config_item_update failed" );

    UNUSED( result );
}

} // namespace plugin
} // namespace kaleidoscope

kaleidoscope::plugin::DynamicMacros DynamicMacros;
