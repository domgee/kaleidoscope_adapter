/*
 * kbdfal_memory.h - This file possibly defines and specifies kaleidoscope_adapter
 *                      long-term memory values and structures to be exposed to
 *                      the high-level application code.
 *
 * Copyright (C) 2025 Dygma Lab S.L. www.dygma.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __KBDFAL_MEMORY_H_
#define __KBDFAL_MEMORY_H_

#ifdef __cplusplus
extern "C" {
#endif

    typedef enum
    {
        KBDMEM_ITEM_TYPE_KEYMAP = 1,
        KBDMEM_ITEM_TYPE_MOUSEKEYS,
        KBDMEM_ITEM_TYPE_KEYROLE,
        KBDMEM_ITEM_TYPE_QUKEYS,
        KBDMEM_ITEM_TYPE_SUPERKEY,
        KBDMEM_ITEM_TYPE_MACROS,
        KBDMEM_ITEM_TYPE_AUTOSHIFT,
        KBDMEM_ITEM_TYPE_CAPSWORD,
        KBDMEM_ITEM_TYPE_COMBOS,
        KBDMEM_ITEM_TYPE_ALTREPEAT,
        KBDMEM_ITEM_TYPE_KEYOVERRIDES,
    } kbdmem_item_type_t;

#ifdef __cplusplus
}
#endif


#endif /* __KBDFAL_MEMORY_H_ */
