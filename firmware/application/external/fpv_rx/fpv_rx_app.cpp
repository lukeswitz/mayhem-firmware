/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
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

#include "fpv_rx_app.hpp"

#include "baseband_api.hpp"

#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"
using namespace portapack;
using namespace tonekey;

#include "audio.hpp"
#include "file.hpp"

#include "utility.hpp"

#include "string_format.hpp"

namespace ui::external_app::fpv_rx {

FpvRxView::FpvRxView(
    NavigationView& nav)
    : nav_(nav) {
    add_children({&rssi,
                  &audio,
                  &field_frequency,
                  &field_lna,
                  &field_vga,
                  &options_modulation,
                  &field_volume,
                  &label_band,
                  &field_band,
                  &label_ch,
                  &field_ch,
                  &text_chan_info,
                  &tv});

    field_frequency.on_show_options = [this]() {
        this->on_show_options_frequency();
    };

    field_lna.on_show_options = [this]() {
        this->on_show_options_rf_gain();
    };

    field_vga.on_show_options = [this]() {
        this->on_show_options_rf_gain();
    };

    options_modulation.set_by_value(toUType(ReceiverModel::Mode::WidebandFMAudio));
    options_modulation.on_change = [this](size_t, OptionsField::value_t v) {
        this->on_modulation_changed(static_cast<ReceiverModel::Mode>(v));
    };
    options_modulation.on_show_options = [this]() {
        this->on_show_options_modulation();
    };

    tv.on_select = [this](int32_t offset) {
        field_frequency.set_value(receiver_model.target_frequency() + offset);
    };

    field_band.on_change = [this](size_t, int32_t v) {
        cur_band_ = static_cast<uint8_t>(v);
        on_channel_changed();
    };

    field_ch.on_change = [this](int32_t v) {
        cur_ch_ = static_cast<uint8_t>(v - 1);
        on_channel_changed();
    };

    tv.on_scan = [this]() {
        if (spectrum_active_) stop_spectrum();
        if (scan_state_ == ScanState::Idle)
            start_scan_mode();
        else
            stop_scan();
    };

    tv.on_spectrum = [this]() {
        if (!spectrum_active_) {
            if (scan_state_ != ScanState::Idle) stop_scan();
            start_spectrum();
        } else {
            stop_spectrum();
        }
    };

    for (uint8_t b = 0; b < FPV_NUM_BANDS; b++)
        for (uint8_t c = 0; c < FPV_CHANNELS_PER_BAND; c++)
            channel_power_[b][c] = -120;

    field_band.set_selected_index(cur_band_);
    field_ch.set_value(cur_ch_ + 1);

    start_video_mode();
    update_chan_info();
}

FpvRxView::~FpvRxView() {
    if (spectrum_active_)
        spectrum_active_ = false;
    audio::output::stop();
    receiver_model.disable();
    baseband::shutdown();
}

void FpvRxView::on_hide() {
    tv.on_hide();
    View::on_hide();
}

void FpvRxView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);
    const ui::Rect tv_rect{0, header_height, new_parent_rect.width(), new_parent_rect.height() - header_height};
    tv.set_parent_rect(tv_rect);
}

void FpvRxView::focus() {
    field_frequency.focus();
}

void FpvRxView::tune_channel(uint8_t band, uint8_t ch) {
    auto freq = fpv_frequencies[band][ch];
    receiver_model.set_target_frequency(freq);
    field_frequency.set_value(freq);
}

void FpvRxView::update_chan_info() {
    char buf[24];
    auto freq_mhz = fpv_frequencies[cur_band_][cur_ch_] / 1000000LL;
    std::snprintf(buf, sizeof(buf), "%c%d %ld MHz",
                  band_labels[cur_band_],
                  static_cast<int>(cur_ch_ + 1),
                  static_cast<long>(freq_mhz));
    text_chan_info.set(buf);
}

void FpvRxView::on_channel_changed() {
    if (scan_state_ != ScanState::Idle || spectrum_active_) return;
    tune_channel(cur_band_, cur_ch_);
    update_chan_info();
}

void FpvRxView::start_video_mode() {
    scan_state_ = ScanState::Idle;
    tv.btn_scan.set_text("SCAN");

    audio::output::mute();
    baseband::shutdown();
    chThdSleepMilliseconds(100);
    baseband::run_prepared_image(portapack::memory::map::m4_code.base());

    receiver_model.set_modulation(ReceiverModel::Mode::WidebandFMAudio);
    receiver_model.set_sampling_rate(10000000);
    receiver_model.set_baseband_bandwidth(10000000);
    receiver_model.set_rf_amp(false);
    receiver_model.set_lna(40);
    receiver_model.set_vga(34);
    receiver_model.enable();

    tune_channel(cur_band_, cur_ch_);

    tv.on_show();
}

void FpvRxView::start_scan_mode() {
    // Crash-free scan: keep the PFPV baseband running, just put it into
    // channel-power mode (cfg idx 10) and sweep the FPV channels by retuning.
    // No baseband image swap -> none of the run_image() hard-fault path.
    baseband::set_sstvrx_phase_slant(FPV_CFG_SCAN_MODE, 1);

    scan_state_ = ScanState::Scanning;
    scan_band_ = 0;
    scan_ch_ = 0;
    verify_samples_ = 0;
    verify_hits_ = 0;
    verify_misses_ = 0;
    verify_sum_db_ = 0;
    verify_peak_db_ = -120;
    candidate_confidence_ = 0;
    lock_hold_ = 0;
    scan_dwell_frames_ = SCAN_DWELL_FRAMES;

    receiver_model.set_target_frequency(fpv_frequencies[scan_band_][scan_ch_]);
    tv.btn_scan.set_text("STOP");
    text_chan_info.set("Scanning...");
}

void FpvRxView::stop_scan() {
    baseband::set_sstvrx_phase_slant(FPV_CFG_SCAN_MODE, 0);  // resume video decode
    scan_state_ = ScanState::Idle;
    tv.btn_scan.set_text("SCAN");
    tune_channel(cur_band_, cur_ch_);
    update_chan_info();
}

void FpvRxView::step_scan() {
    scan_ch_ = static_cast<uint8_t>(scan_ch_ + 1);
    if (scan_ch_ >= FPV_CHANNELS_PER_BAND) {
        scan_ch_ = 0;
        scan_band_ = static_cast<uint8_t>(scan_band_ + 1);
        if (scan_band_ >= FPV_NUM_BANDS)
            scan_band_ = 0;
    }
    receiver_model.set_target_frequency(fpv_frequencies[scan_band_][scan_ch_]);
    scan_dwell_frames_ = SCAN_DWELL_FRAMES;

    char buf[24];
    std::snprintf(buf, sizeof(buf), "Scan %c%d...",
                  band_labels[scan_band_],
                  static_cast<int>(scan_ch_ + 1));
    text_chan_info.set(buf);
}

void FpvRxView::scan_step() {
    scan_ch_ = static_cast<uint8_t>(scan_ch_ + 1);
    if (scan_ch_ >= FPV_CHANNELS_PER_BAND) {
        scan_ch_ = 0;
        scan_band_ = static_cast<uint8_t>(scan_band_ + 1);
        if (scan_band_ >= FPV_NUM_BANDS) {
            scan_band_ = 0;
            // Full sweep complete: lock the strongest channel if it clearly beats the weakest.
            uint8_t best_b = 0, best_c = 0;
            int16_t best = -127, worst = 127;
            for (uint8_t b = 0; b < FPV_NUM_BANDS; b++)
                for (uint8_t c = 0; c < FPV_CHANNELS_PER_BAND; c++) {
                    const int16_t p = channel_power_[b][c];
                    if (p > best) {
                        best = p;
                        best_b = b;
                        best_c = c;
                    }
                    if (p < worst) worst = p;
                }
            if (best - worst >= SCAN_LOCK_MARGIN_DB) {
                scan_band_ = best_b;
                scan_ch_ = best_c;
                enter_lock();
                return;
            }
            // No clear peak this sweep -> keep scanning.
        }
    }
    receiver_model.set_target_frequency(fpv_frequencies[scan_band_][scan_ch_]);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Scan %c%d...",
                  band_labels[scan_band_],
                  static_cast<int>(scan_ch_ + 1));
    text_chan_info.set(buf);
}

void FpvRxView::on_statistics_update(const ChannelStatistics& statistics) {
    if (spectrum_active_) {
        channel_power_[spec_scan_band_][spec_scan_ch_] = statistics.max_db;
        spec_step();
        return;
    }

    if (scan_state_ == ScanState::Idle) return;

    switch (scan_state_) {
        case ScanState::Scanning:
            // sweep-and-lock-strongest: record this channel's avg power, then advance.
            channel_power_[scan_band_][scan_ch_] = static_cast<int16_t>(statistics.max_db);
            scan_step();
            break;
        case ScanState::Candidate:
            evaluate_candidate(statistics);
            break;
        case ScanState::Locked:
            update_lock(statistics);
            break;
        default:
            break;
    }
}

void FpvRxView::enter_candidate(const ChannelStatistics& statistics) {
    scan_state_ = ScanState::Candidate;
    verify_samples_ = 1;
    verify_hits_ = 1;
    verify_misses_ = 0;
    verify_sum_db_ = statistics.max_db;
    verify_peak_db_ = statistics.max_db;
    candidate_confidence_ = 0;

    char buf[24];
    std::snprintf(buf, sizeof(buf), "Check %c%d...",
                  band_labels[scan_band_],
                  static_cast<int>(scan_ch_ + 1));
    text_chan_info.set(buf);

    baseband::request_audio_beep(1150, 24000, 60);
}

void FpvRxView::evaluate_candidate(const ChannelStatistics& statistics) {
    ++verify_samples_;
    verify_sum_db_ += statistics.max_db;
    if (statistics.max_db > verify_peak_db_)
        verify_peak_db_ = statistics.max_db;

    if (statistics.max_db >= detect_threshold_db_)
        ++verify_hits_;
    else
        ++verify_misses_;

    int32_t avg_db = verify_samples_ ? (verify_sum_db_ / verify_samples_) : -120;
    int32_t center_margin = avg_db - detect_threshold_db_;
    int32_t peak_margin = verify_peak_db_ - detect_threshold_db_;

    int32_t score = 18;
    score += center_margin * 7;
    score += peak_margin * 4;
    score += static_cast<int32_t>(verify_hits_) * 5;
    score -= static_cast<int32_t>(verify_misses_) * 10;
    if (score < 0) score = 0;
    if (score > 99) score = 99;
    candidate_confidence_ = static_cast<uint8_t>(score);

    bool strong_lock =
        verify_samples_ >= 3 &&
        verify_hits_ >= 3 &&
        verify_peak_db_ >= (detect_threshold_db_ + STRONG_LOCK_PEAK_MARGIN_DB) &&
        candidate_confidence_ >= 80;

    if (strong_lock) {
        enter_lock();
        return;
    }

    if (verify_samples_ >= VERIFY_SAMPLE_TARGET) {
        if (verify_hits_ >= VERIFY_MIN_HITS && candidate_confidence_ >= LOCK_CONFIDENCE_MIN) {
            enter_lock();
        } else {
            scan_state_ = ScanState::Scanning;
            verify_samples_ = 0;
            verify_hits_ = 0;
            verify_misses_ = 0;
            verify_sum_db_ = 0;
            verify_peak_db_ = -120;
            scan_dwell_frames_ = 0;
            text_chan_info.set("Scanning...");
        }
    }
}

void FpvRxView::enter_lock() {
    cur_band_ = scan_band_;
    cur_ch_ = scan_ch_;

    field_band.set_selected_index(cur_band_);
    field_ch.set_value(cur_ch_ + 1);

    char buf[24];
    std::snprintf(buf, sizeof(buf), "LOCKED %c%d %ldMHz",
                  band_labels[cur_band_],
                  static_cast<int>(cur_ch_ + 1),
                  static_cast<long>(fpv_frequencies[cur_band_][cur_ch_] / 1000000LL));
    tv.btn_scan.set_text("SCAN");
    text_chan_info.set(buf);

    // Resume video decode on the locked channel (PFPV never stopped running).
    baseband::set_sstvrx_phase_slant(FPV_CFG_SCAN_MODE, 0);
    scan_state_ = ScanState::Idle;
    tune_channel(cur_band_, cur_ch_);
}

void FpvRxView::update_lock(const ChannelStatistics& statistics) {
    int16_t unlock_th = static_cast<int16_t>(detect_threshold_db_ - 6);

    if (statistics.max_db >= unlock_th) {
        if (lock_hold_ < LOCK_HOLD_MAX) ++lock_hold_;
    } else {
        if (lock_hold_ > 0) --lock_hold_;
    }

    if (lock_hold_ == 0) {
        scan_state_ = ScanState::Scanning;
        verify_samples_ = 0;
        verify_hits_ = 0;
        verify_misses_ = 0;
        verify_sum_db_ = 0;
        verify_peak_db_ = -120;
        scan_dwell_frames_ = 0;
        text_chan_info.set("Scanning...");
        baseband::request_audio_beep(650, 24000, 90);
    }
}

void FpvRxView::start_spectrum() {
    spectrum_active_ = true;
    spec_scan_band_ = 0;
    spec_scan_ch_ = 0;
    spec_sweep_done_ = false;

    for (uint8_t b = 0; b < FPV_NUM_BANDS; b++)
        for (uint8_t c = 0; c < FPV_CHANNELS_PER_BAND; c++)
            channel_power_[b][c] = -120;

    // Crash-free spectrum sweep: PFPV stays running in channel-power mode; we just
    // retune across every channel and collect ChannelStatistics. No image swap.
    baseband::set_sstvrx_phase_slant(FPV_CFG_SCAN_MODE, 1);
    receiver_model.set_target_frequency(fpv_frequencies[0][0]);

    tv.btn_spectrum.set_text("STOP");
    text_chan_info.set("Spectrum sweep...");
}

void FpvRxView::stop_spectrum() {
    spectrum_active_ = false;
    baseband::set_sstvrx_phase_slant(FPV_CFG_SCAN_MODE, 0);  // resume video decode
    tv.btn_spectrum.set_text("Spectrum");
    tune_channel(cur_band_, cur_ch_);
    update_chan_info();
}

void FpvRxView::spec_step() {
    spec_scan_ch_++;
    if (spec_scan_ch_ >= FPV_CHANNELS_PER_BAND) {
        spec_scan_ch_ = 0;
        spec_scan_band_++;
        if (spec_scan_band_ >= FPV_NUM_BANDS) {
            spec_scan_band_ = 0;
            spec_sweep_done_ = true;
            draw_spectrum_bars();
        }
    }
    receiver_model.set_target_frequency(fpv_frequencies[spec_scan_band_][spec_scan_ch_]);
}

void FpvRxView::draw_spectrum_bars() {
    if (!spec_sweep_done_) return;

    auto& display = portapack::display;

    const int16_t bar_area_y = header_height + 48 + 16;
    const int16_t bar_area_h = 320 - bar_area_y - 20;

    // Auto-range to the measured powers so the strongest channel is full height and
    // weak channels are short, regardless of the absolute dB scale.
    int16_t db_floor = 127, db_ceil = -127;
    for (uint8_t b = 0; b < FPV_NUM_BANDS; b++)
        for (uint8_t c = 0; c < FPV_CHANNELS_PER_BAND; c++) {
            const int16_t p = channel_power_[b][c];
            if (p < db_floor) db_floor = p;
            if (p > db_ceil) db_ceil = p;
        }
    if (db_ceil - db_floor < 6) db_ceil = static_cast<int16_t>(db_floor + 6);

    display.fill_rectangle({{0, bar_area_y}, {240, static_cast<ui::Dim>(bar_area_h + 20)}}, Color::black());

    for (uint8_t b = 0; b < FPV_NUM_BANDS; b++) {
        int16_t group_x = static_cast<int16_t>(b * 48);

        for (uint8_t c = 0; c < FPV_CHANNELS_PER_BAND; c++) {
            int16_t x = group_x + static_cast<int16_t>(c * 6);
            int16_t db = channel_power_[b][c];
            if (db < db_floor) db = db_floor;
            if (db > db_ceil) db = db_ceil;

            int16_t h = static_cast<int16_t>(
                static_cast<int32_t>(db - db_floor) * bar_area_h / (db_ceil - db_floor));
            if (h < 1) h = 1;

            auto color = Color::green();
            if (b == cur_band_ && c == cur_ch_) color = Color::yellow();

            display.fill_rectangle({{x, static_cast<int16_t>(bar_area_y + bar_area_h - h)},
                                    {5, static_cast<ui::Dim>(h)}},
                                   color);
        }

        if (b < FPV_NUM_BANDS - 1) {
            int16_t sep_x = static_cast<int16_t>(group_x + 48 - 1);
            display.draw_line({sep_x, bar_area_y}, {sep_x, static_cast<int16_t>(bar_area_y + bar_area_h)}, Color::dark_grey());
        }
    }
}

void FpvRxView::remove_options_widget() {
    if (options_widget) {
        remove_child(options_widget.get());
        options_widget.reset();
    }

    field_lna.set_style(nullptr);
    options_modulation.set_style(nullptr);
    field_frequency.set_style(nullptr);
}

void FpvRxView::set_options_widget(std::unique_ptr<Widget> new_widget) {
    remove_options_widget();

    if (new_widget) {
        options_widget = std::move(new_widget);
    } else {
        options_widget = std::make_unique<Rectangle>(options_view_rect, Theme::getInstance()->option_active->background);
    }
    add_child(options_widget.get());
}

void FpvRxView::on_show_options_frequency() {
    auto widget = std::make_unique<FrequencyOptionsView>(options_view_rect, Theme::getInstance()->option_active);

    widget->set_step(receiver_model.frequency_step());
    widget->on_change_step = [this](rf::Frequency f) {
        this->on_frequency_step_changed(f);
    };
    widget->set_reference_ppm_correction(persistent_memory::correction_ppb() / 1000);
    widget->on_change_reference_ppm_correction = [this](int32_t v) {
        this->on_reference_ppm_correction_changed(v);
    };

    set_options_widget(std::move(widget));
    field_frequency.set_style(Theme::getInstance()->option_active);
}

void FpvRxView::on_show_options_rf_gain() {
    auto widget = std::make_unique<RadioGainOptionsView>(options_view_rect, Theme::getInstance()->option_active);
    set_options_widget(std::move(widget));
    field_lna.set_style(Theme::getInstance()->option_active);
}

void FpvRxView::on_show_options_modulation() {
    std::unique_ptr<Widget> widget;

    static_cast<ReceiverModel::Mode>(receiver_model.modulation());
    tv.show_audio_spectrum_view(false);

    set_options_widget(std::move(widget));
    options_modulation.set_style(Theme::getInstance()->option_active);
}

void FpvRxView::on_frequency_step_changed(rf::Frequency f) {
    receiver_model.set_frequency_step(f);
    field_frequency.set_step(f);
}

void FpvRxView::on_reference_ppm_correction_changed(int32_t v) {
    persistent_memory::set_correction_ppb(v * 1000);
}

void FpvRxView::update_modulation(const ReceiverModel::Mode modulation) {
    audio::output::mute();

    baseband::shutdown();

    baseband::run_prepared_image(portapack::memory::map::m4_code.base());

    receiver_model.set_modulation(modulation);
    receiver_model.set_sampling_rate(10000000);
    receiver_model.set_baseband_bandwidth(10000000);
    receiver_model.set_rf_amp(false);
    receiver_model.set_lna(40);
    receiver_model.set_vga(34);
    receiver_model.enable();
}

void FpvRxView::on_freqchg(int64_t freq) {
    field_frequency.set_value(freq);
}

void FpvRxView::on_baseband_bandwidth_changed(uint32_t bandwidth_hz) {
    receiver_model.set_baseband_bandwidth(bandwidth_hz);
}

void FpvRxView::on_modulation_changed(const ReceiverModel::Mode modulation) {
    tv.on_hide();
    update_modulation(modulation);
    on_show_options_modulation();
    tv.on_show();
}

}  // namespace ui::external_app::fpv_rx
