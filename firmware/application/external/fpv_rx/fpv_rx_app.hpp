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

#ifndef __FPV_RX_APP_H__
#define __FPV_RX_APP_H__

#include "receiver_model.hpp"

#include "ui_receiver.hpp"
#include "ui_freq_field.hpp"
#include "ui_fpv_tv.hpp"
#include "ui_record_view.hpp"
#include "app_settings.hpp"
#include "radio_state.hpp"

#include "tone_key.hpp"
#include "oversample.hpp"
#include "ui_spectrum.hpp"

#include <cstdio>

namespace ui::external_app::fpv_rx {

enum class ScanState : uint8_t {
    Idle = 0,
    Scanning,
    Candidate,
    Locked,
};

static constexpr uint8_t FPV_NUM_BANDS = 5;
static constexpr uint8_t FPV_CHANNELS_PER_BAND = 8;

// PFPV baseband config index (rides SSTVRXPhaseSlant) toggling channel-power scan mode
// instead of switching baseband images. Keep in sync with proc_fpv_rx apply_cfg case 10.
static constexpr int FPV_CFG_SCAN_MODE = 10;

static constexpr int64_t fpv_frequencies[FPV_NUM_BANDS][FPV_CHANNELS_PER_BAND] = {
    /* A */ {5865000000LL, 5845000000LL, 5825000000LL, 5805000000LL, 5785000000LL, 5765000000LL, 5745000000LL, 5725000000LL},
    /* B */ {5733000000LL, 5752000000LL, 5771000000LL, 5790000000LL, 5809000000LL, 5828000000LL, 5847000000LL, 5866000000LL},
    /* E */ {5705000000LL, 5685000000LL, 5665000000LL, 5645000000LL, 5885000000LL, 5905000000LL, 5925000000LL, 5945000000LL},
    /* F */ {5740000000LL, 5760000000LL, 5780000000LL, 5800000000LL, 5820000000LL, 5840000000LL, 5860000000LL, 5880000000LL},
    /* R */ {5658000000LL, 5695000000LL, 5732000000LL, 5769000000LL, 5806000000LL, 5843000000LL, 5880000000LL, 5917000000LL},
};

static constexpr char band_labels[] = {'A', 'B', 'E', 'F', 'R'};

class FpvRxView : public View {
   public:
    FpvRxView(NavigationView& nav);
    ~FpvRxView();

    void on_hide() override;

    void set_parent_rect(const Rect new_parent_rect) override;

    void focus() override;

    std::string title() const override { return "FPV-RX"; };

   private:
    static constexpr ui::Dim header_height = 3 * 16;

    NavigationView& nav_;
    RxRadioState radio_state_{};
    app_settings::SettingsManager settings_{
        "rx_fpv_rx", app_settings::Mode::RX};

    static constexpr uint32_t FPV_SCAN_RX_BW = 750000;
    static constexpr int16_t detect_threshold_db_ = -38;
    static constexpr uint8_t SCAN_DWELL_FRAMES = 1;
    static constexpr uint8_t VERIFY_SAMPLE_TARGET = 5;
    static constexpr uint8_t VERIFY_MIN_HITS = 4;
    static constexpr int16_t STRONG_LOCK_PEAK_MARGIN_DB = 8;
    static constexpr uint8_t LOCK_HOLD_MAX = 12;
    static constexpr uint8_t LOCK_CONFIDENCE_MIN = 72;
    static constexpr int16_t SCAN_LOCK_MARGIN_DB = 6;  // strongest channel must beat weakest by this to lock

    const Rect options_view_rect{UI_POS_X(0), 1 * 16, screen_width, 1 * 16};

    uint8_t cur_band_{0};
    uint8_t cur_ch_{7};

    ScanState scan_state_{ScanState::Idle};
    uint8_t scan_band_{0};
    uint8_t scan_ch_{0};
    uint8_t scan_dwell_frames_{0};
    uint8_t verify_samples_{0};
    uint8_t verify_hits_{0};
    uint8_t verify_misses_{0};
    int32_t verify_sum_db_{0};
    int16_t verify_peak_db_{-120};
    uint8_t candidate_confidence_{0};
    uint8_t lock_hold_{0};

    bool spectrum_active_{false};
    uint8_t spec_scan_band_{0};
    uint8_t spec_scan_ch_{0};
    bool spec_sweep_done_{false};
    int16_t channel_power_[FPV_NUM_BANDS][FPV_CHANNELS_PER_BAND]{};

    RSSI rssi{
        {21 * 8, 0, 6 * 8, 4}};

    Audio audio{
        {21 * 8, 10, 6 * 8, 4}};

    RxFrequencyField field_frequency{
        {5 * 8, UI_POS_Y(0)},
        nav_};

    LNAGainField field_lna{
        {15 * 8, UI_POS_Y(0)}};

    VGAGainField field_vga{
        {18 * 8, UI_POS_Y(0)}};

    OptionsField options_modulation{
        {UI_POS_X(0), UI_POS_Y(0)},
        4,
        {
            {"FPV", toUType(ReceiverModel::Mode::WidebandFMAudio)},
            {"FPV", toUType(ReceiverModel::Mode::WidebandFMAudio)},
            {"FPV", toUType(ReceiverModel::Mode::WidebandFMAudio)},
        }};

    AudioVolumeField field_volume{
        {27 * 8, UI_POS_Y(0)}};

    Text label_band{
        {0, UI_POS_Y(2), 2 * 8, 16},
        "B:"};

    OptionsField field_band{
        {2 * 8, UI_POS_Y(2)},
        1,
        {
            {"A", 0},
            {"B", 1},
            {"E", 2},
            {"F", 3},
            {"R", 4},
        }};

    Text label_ch{
        {4 * 8, UI_POS_Y(2), 1 * 8, 16},
        ":"};

    NumberField field_ch{
        {5 * 8, UI_POS_Y(2)},
        1,
        {1, 8},
        1,
        ' '};

    Text text_chan_info{
        {7 * 8, UI_POS_Y(2), 20 * 8, 16},
        ""};

    std::unique_ptr<Widget> options_widget{};

    tv::TVWidget tv{};

    void tune_channel(uint8_t band, uint8_t ch);
    void update_chan_info();
    void on_channel_changed();

    void start_video_mode();
    void start_scan_mode();
    void stop_scan();
    void step_scan();
    void scan_step();  // sweep-and-lock-strongest scan stepper
    void on_statistics_update(const ChannelStatistics& statistics);
    void enter_candidate(const ChannelStatistics& statistics);
    void evaluate_candidate(const ChannelStatistics& statistics);
    void enter_lock();
    void update_lock(const ChannelStatistics& statistics);

    void start_spectrum();
    void stop_spectrum();
    void spec_step();
    void draw_spectrum_bars();

    void on_baseband_bandwidth_changed(uint32_t bandwidth_hz);
    void on_modulation_changed(const ReceiverModel::Mode modulation);
    void on_show_options_frequency();
    void on_show_options_rf_gain();
    void on_show_options_modulation();
    void on_frequency_step_changed(rf::Frequency f);
    void on_reference_ppm_correction_changed(int32_t v);

    void remove_options_widget();
    void set_options_widget(std::unique_ptr<Widget> new_widget);

    void update_modulation(const ReceiverModel::Mode modulation);

    MessageHandlerRegistration message_handler_freqchg{
        Message::ID::FreqChangeCommand,
        [this](Message* const p) {
            const auto message = static_cast<const FreqChangeCommandMessage*>(p);
            this->field_frequency.set_value(message->freq);
        }};

    MessageHandlerRegistration message_handler_statistics{
        Message::ID::ChannelStatistics,
        [this](Message* const p) {
            const auto message = static_cast<const ChannelStatisticsMessage*>(p);
            this->on_statistics_update(message->statistics);
        }};

    void on_freqchg(int64_t freq);
};

}  // namespace ui::external_app::fpv_rx

#endif /*__FPV_RX_APP_H__*/
