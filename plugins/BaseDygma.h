/* BaseDygma - Dygma base plugin for Kaleidoscope
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
#include "kaleidoscope/plugin.h"

namespace kaleidoscope {
namespace plugin {

class BaseDygma : public Plugin
{

  public:
    BaseDygma(void);

    kaleidoscope::EventHandlerResult beforeReportingState();

    result_t key_report_enable( void );
    result_t key_report_disable( void );

    /* False while the keyboard API holds key reports off, such as during BLE pairing */
    bool isKeyReportEnabled() const {
      return key_report_enabled;
    }

  private:

    bool_t key_report_enabled = true;

};

} // namespace plugin
} // namespace kaleidoscope

extern kaleidoscope::plugin::BaseDygma BaseDygma;
