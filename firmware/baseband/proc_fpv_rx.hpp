/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2026 FPV-RX
 *
 * Analog-FPV NTSC decoder, BURST-CAPTURE (the proven recipe). execute() memcpy's
 * raw 10 MHz c8 IQ into a RAM burst; when full it is demodulated OFFLINE with
 * PER-SAMPLE atan2 (no real-time budget wall, no collapse-to-zero from averaging
 * complex before the angle) -> 5-tap LPF -> decimate -> histogram-percentile
 * levels -> sync-width line FSM -> painted row-by-row. DMA during the offline
 * decode is dropped (the frame re-locks on the next vertical sync).
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

#ifndef __PROC_FPV_RX_H__
#define __PROC_FPV_RX_H__

#include "baseband_processor.hpp"
#include "baseband_thread.hpp"
#include "rssi_thread.hpp"

#include "dsp_types.hpp"
#include "message.hpp"

#include <array>
#include <cstdint>

class FpvRxProcessor : public BasebandProcessor {
   public:
    void execute(const buffer_c8_t& buffer) override;
    void on_message(const Message* const message) override;

   private:
    static constexpr size_t baseband_fs = 10000000;  // proven: 10 MHz capture, centered on the channel
    static constexpr int DECIM = 5;                  // -> fsm_fs = 2 MHz
    static constexpr size_t fsm_fs = baseband_fs / DECIM;

    static constexpr int video_width = 240;
    static constexpr int video_height = 240;
    static constexpr int vsync_debounce = 180;
    static constexpr int hsync_w = (int)(4.7e-6 * fsm_fs);

    static constexpr uint32_t line_bb = baseband_fs / 15734;
    static constexpr uint32_t cyc_per_line = (200000000u / baseband_fs) * line_bb;

    int cfg_field_lines{262};
    int cfg_gap_trim{0};
    int cfg_sync_th_bias{0};
    int cfg_black_pct{25};
    int cfg_white_pct{99};
    int cfg_field_w{(int)(18e-6 * fsm_fs)};
    int cfg_video_w{(int)(52.6e-6 * fsm_fs)};
    int cfg_backporch_w{(int)(4.7e-6 * fsm_fs)};
    int cfg_hsync_lo{1};
    int cfg_hsync_hi{85};

    int low_run{0};
    bool capturing{false};
    int line_pos{0};
    int disp_row{0};
    int since_vsync{99999};

    // ---- histogram-percentile levels (proven): per-burst, outlier-robust ----
    int32_t lvl_sync{0};
    int32_t lvl_black{0};
    int32_t lvl_white{1};
    int32_t lvl_sync_th{0};

    uint8_t line_buf[video_width]{};

    // BURST CAPTURE: 24576 c8 samples ~= 2.46 ms ~= 38 NTSC lines (48 KB).
    static constexpr size_t burst_samples = 24576;
    uint8_t burst[burst_samples * 2]{};
    size_t burst_pos{0};

    // Decoded lines -> M0 painter.
    static constexpr size_t fifo_k = 6;  // 64 slots
    ChannelSpectrum fifo_data[1 << fifo_k]{};
    ChannelSpectrumFIFO fifo{fifo_data, fifo_k};

    bool configured{false};
    bool streaming{false};
    bool scan_mode{false};  // cfg idx 10: report channel power instead of decoding (crash-free scan/spectrum, no baseband image swap)
    uint64_t scan_acc{0};   // accumulated |IQ|^2 for average-power channel measurement
    uint32_t scan_cnt{0};

    uint32_t peak_permille{0};  // header self-label: packed (black<<16 | sync&0xFFFF)
    uint32_t field_count{0};

    void process_sample(int32_t s);
    void advance_row();
    void apply_cfg(int idx, int val);
    void emit_line(int row);
    void decode_burst();
    void on_spectrum_streaming(const SpectrumStreamingConfigMessage& message);

    /* NB: Threads should be the last members in the class definition. */
    BasebandThread baseband_thread{baseband_fs, this, baseband::Direction::Receive};
    RSSIThread rssi_thread{};
};

#endif /*__PROC_FPV_RX_H__*/
