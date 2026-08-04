/*
 * Copyright (C) 2023 Bernd Herzog
 * Copyright (C) 2026 FPV-RX
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui.hpp"
#include "fpv_rx_app.hpp"
#include "ui_navigation.hpp"
#include "external_app.hpp"

namespace ui::external_app::fpv_rx {
void initialize_app(ui::NavigationView& nav) {
    nav.push<FpvRxView>();
}
}  // namespace ui::external_app::fpv_rx

extern "C" {

__attribute__((section(".external_app.app_fpv_rx.application_information"), used)) application_information_t _application_information_fpv_rx = {
    /*.memory_location = */ (uint8_t*)0x00000000,
    /*.externalAppEntry = */ ui::external_app::fpv_rx::initialize_app,
    /*.header_version = */ CURRENT_HEADER_VERSION,
    /*.app_version = */ VERSION_MD5,

    /*.app_name = */ "FPV-RX",
    /*.bitmap_data = */ {
        0x00,
        0x00,
        0xE0,
        0x01,
        0x20,
        0x01,
        0xFE,
        0x7F,
        0x02,
        0x40,
        0xC2,
        0x43,
        0x22,
        0x44,
        0xA2,
        0x45,
        0xA2,
        0x45,
        0x22,
        0x44,
        0xC2,
        0x43,
        0x02,
        0x40,
        0xFE,
        0x7F,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    },
    /*.icon_color = */ ui::Color::cyan().v,
    /*.menu_location = */ app_location_t::RX,
    /*.desired_menu_position = */ -1,

    /*.m4_app_tag = portapack::spi_flash::image_tag_fpv_rx */ {'P', 'F', 'P', 'V'},
    /*.m4_app_offset = */ 0x00000000,  // will be filled at compile time
};
}
