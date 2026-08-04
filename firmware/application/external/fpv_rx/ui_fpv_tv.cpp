/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2020 Shao
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

#include "ui_fpv_tv.hpp"

#include "spectrum_color_lut.hpp"

#include "portapack.hpp"
using namespace portapack;

#include "baseband_api.hpp"

#include "string_format.hpp"

#include <cmath>
#include <array>

namespace ui::external_app::fpv_rx {
namespace tv {

/* TimeScopeView******************************************************/

TimeScopeView::TimeScopeView(
    const Rect parent_rect)
    : View{parent_rect} {
    set_focusable(true);

    add_children({&waveform});
}

void TimeScopeView::paint(Painter& painter) {
    const auto r = screen_rect();

    painter.fill_rectangle(r, Color::black());
}

void TimeScopeView::on_audio_spectrum(const AudioSpectrum* spectrum) {
    for (size_t i = 0; i < spectrum->db.size(); i++)
        audio_spectrum[i] = ((int16_t)spectrum->db[i] - 127) * 256;
    waveform.set_dirty();
}

/* TVView *********************************************************/

void TVView::on_show() {
    clear();
}

void TVView::on_hide() {
    display.scroll_disable();
}

void TVView::paint(Painter& painter) {
    // Do nothing.
    (void)painter;
}

void TVView::on_adjust_xcorr(uint8_t xcorr) {
    x_correction = xcorr;
}

void TVView::on_channel_spectrum(
    const ChannelSpectrum& spectrum) {
    // The baseband decoder ships one sync-aligned, contrast-mapped video line
    // per callback, tagged with its target screen row in channel_filter_low_frequency.
    const auto r = screen_rect();
    const int y = spectrum.channel_filter_low_frequency;
    if (y < 0 || y >= r.height())
        return;

    int w = r.width();
    if (w > 240)
        w = 240;

    ui::Color line_buffer[240];
    for (int px = 0; px < w; px++) {
        const uint8_t d = spectrum.db[px];
        line_buffer[px] = ui::Color(d, d, d);
    }

    display.render_line({r.left(), (Coord)(r.top() + y)}, w, line_buffer);
}

void TVView::clear() {
    display.fill_rectangle(
        screen_rect(),
        Color::black());
}

/* TVWidget *******************************************************/

TVWidget::TVWidget() {
    add_children({&tv_view,
                  &labels,
                  &field_vhold,
                  &field_trim,
                  &field_sync,
                  &field_blk,
                  &field_wht,
                  &field_fldw,
                  &field_vidw,
                  &field_bpor,
                  &field_hslo,
                  &field_hshi,
                  &btn_scan,
                  &btn_spectrum});
    btn_scan.on_select = [this](Button&) {
        if (on_scan) on_scan();
    };
    btn_spectrum.on_select = [this](Button&) {
        if (on_spectrum) on_spectrum();
    };
    field_vhold.set_value(262);
    field_trim.set_value(0);
    field_sync.set_value(0);
    field_blk.set_value(25);
    field_wht.set_value(99);
    field_fldw.set_value(36);
    field_vidw.set_value(105);
    field_bpor.set_value(9);
    field_hslo.set_value(1);
    field_hshi.set_value(85);
    auto cb = [this](int32_t) { this->send_config(); };
    field_vhold.on_change = cb;
    field_trim.on_change = cb;
    field_sync.on_change = cb;
    field_blk.on_change = cb;
    field_wht.on_change = cb;
    field_fldw.on_change = cb;
    field_vidw.on_change = cb;
    field_bpor.on_change = cb;
    field_hslo.on_change = cb;
    field_hshi.on_change = cb;
}

void TVWidget::send_config() {
    baseband::set_sstvrx_phase_slant(0, field_vhold.value());
    baseband::set_sstvrx_phase_slant(1, field_trim.value());
    baseband::set_sstvrx_phase_slant(2, field_sync.value());
    baseband::set_sstvrx_phase_slant(3, field_blk.value());
    baseband::set_sstvrx_phase_slant(4, field_wht.value());
    baseband::set_sstvrx_phase_slant(5, field_fldw.value());
    baseband::set_sstvrx_phase_slant(6, field_vidw.value());
    baseband::set_sstvrx_phase_slant(7, field_bpor.value());
    baseband::set_sstvrx_phase_slant(8, field_hslo.value());
    baseband::set_sstvrx_phase_slant(9, field_hshi.value());
}

void TVWidget::on_show() {
    baseband::spectrum_streaming_start();
}

void TVWidget::on_hide() {
    baseband::spectrum_streaming_stop();
    channel_fifo = nullptr;
}

void TVWidget::show_audio_spectrum_view(const bool show) {
    if ((audio_spectrum_view && show) || (!audio_spectrum_view && !show)) return;

    if (show) {
        audio_spectrum_view = std::make_unique<TimeScopeView>(audio_spectrum_view_rect);
        add_child(audio_spectrum_view.get());
        update_widgets_rect();
    } else {
        audio_spectrum_update = false;
        remove_child(audio_spectrum_view.get());
        audio_spectrum_view.reset();
        update_widgets_rect();
    }
}

void TVWidget::update_widgets_rect() {
    if (audio_spectrum_view) {
        tv_view.set_parent_rect(tv_reduced_rect);
    } else {
        tv_view.set_parent_rect(tv_normal_rect);
    }
    tv_view.on_show();
}

void TVWidget::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    tv_normal_rect = {0, scale_height, new_parent_rect.width(), new_parent_rect.height() - scale_height};
    tv_reduced_rect = {0, audio_spectrum_height + scale_height, new_parent_rect.width(), new_parent_rect.height() - scale_height - audio_spectrum_height};

    update_widgets_rect();
}

void TVWidget::paint(Painter& painter) {
    // TODO:
    (void)painter;
}

void TVWidget::on_channel_spectrum(const ChannelSpectrum& spectrum) {
    tv_view.on_channel_spectrum(spectrum);
    sampling_rate = spectrum.sampling_rate;

    (void)stats_div;
}

void TVWidget::on_audio_spectrum() {
    audio_spectrum_view->on_audio_spectrum(audio_spectrum_data);
}

} /* namespace tv */
}  // namespace ui::external_app::fpv_rx
