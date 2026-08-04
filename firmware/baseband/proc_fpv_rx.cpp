/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2026 FPV-RX
 *
 * Analog-FPV NTSC decoder, BURST-CAPTURE (proven recipe). See proc_fpv_rx.hpp.
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

#include "proc_fpv_rx.hpp"

#include "portapack_shared_memory.hpp"
#include "event_m4.hpp"
#include "lpc43xx_cpp.hpp"
#include "utility.hpp"  // mag2_to_dbv_norm

#include <hal.h>

#include <array>
#include <cstring>
#include <memory>

namespace {
// Division-free fixed-point atan2 (no SDIV). Output [-32768,32767] == [-pi,pi).
constexpr std::array<uint16_t, 256> make_recip_lut() {
    std::array<uint16_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++)
        t[i] = (uint16_t)((1u << 23) / (256u + i));
    return t;
}
constexpr std::array<uint16_t, 256> recip_lut = make_recip_lut();

static inline int q15_mul_(int j, int k) {
    const int t = j * k;
    return (t + (((t & 0x7FFF) == 0x4000) ? 0 : 0x4000)) >> 15;
}

static inline int fpv_atan2(int y, int x) {
    if (x == 0 && y == 0) return 0;
    const int ax = x < 0 ? -x : x;
    const int ay = y < 0 ? -y : y;
    const bool swap = ay > ax;
    const uint32_t num = swap ? (uint32_t)ax : (uint32_t)ay;
    const uint32_t den = swap ? (uint32_t)ay : (uint32_t)ax;
    const int e = 31 - __builtin_clz(den);
    uint32_t m = (e >= 8) ? (den >> (e - 8)) : (den << (8 - e));
    if (m > 511)
        m = 511;
    else if (m < 256)
        m = 256;
    uint32_t ratio = (num * recip_lut[m - 256] + (e ? (1u << (e - 1)) : 0u)) >> e;
    if (ratio > 32767) ratio = 32767;
    const int corr = q15_mul_(2847, -(int)ratio);
    const int base = q15_mul_(11039 + corr, (int)ratio);
    const int ang = swap ? (16384 - base) : base;
    int turn = (x >= 0) ? ((y >= 0) ? ang : (65536 - ang))
                        : ((y >= 0) ? (32768 - ang) : (32768 + ang));
    if (turn >= 32768) turn -= 65536;
    return turn;
}

// Value at the given percentile of a 256-bin histogram (bins span [-32768,32767]).
static int32_t pct_level(const uint16_t* hist, int total, int pct) {
    const int target = (int)((int64_t)total * pct / 100);
    int cum = 0;
    for (int b = 0; b < 256; b++) {
        cum += hist[b];
        if (cum >= target)
            return ((int32_t)b << 8) - 32768;
    }
    return 32767;
}

static volatile uint32_t* const dwt_cyccnt = reinterpret_cast<uint32_t*>(0xE0001004u);
static volatile uint32_t* const dwt_ctrl = reinterpret_cast<uint32_t*>(0xE0001000u);
static volatile uint32_t* const scb_demcr = reinterpret_cast<uint32_t*>(0xE000EDFCu);
}  // namespace

void FpvRxProcessor::emit_line(int row) {
    if (!streaming || fifo.is_full())
        return;
    ChannelSpectrum spec;
    spec.sampling_rate = baseband_fs;
    spec.channel_filter_low_frequency = row;
    spec.channel_filter_high_frequency = (int32_t)peak_permille;
    spec.channel_filter_transition = (int32_t)field_count;
    for (int i = 0; i < video_width; i++)
        spec.db[i] = line_buf[i];
    fifo.in(spec);
}

// One decimated luma sample -> sync-width line FSM. A sync is a low run: a short
// run (~hsync) starts a line; a long run (> field_w) is vertical sync -> top of
// frame. Active video is mapped black..white -> 0..255 with zero-order-hold fill.
void FpvRxProcessor::process_sample(int32_t s) {
    int32_t wr = lvl_white - lvl_black;
    if (wr < 1)
        wr = 1;

    if (capturing) {
        if (line_pos >= 0) {
            int px0 = (line_pos * video_width) / cfg_video_w;
            int px1 = ((line_pos + 1) * video_width) / cfg_video_w;
            if (px0 < 0) px0 = 0;
            if (px1 > video_width) px1 = video_width;
            int32_t bw = ((s - lvl_black) * 255) / wr;
            const uint8_t val = (uint8_t)(bw < 0 ? 0 : (bw > 255 ? 255 : bw));
            for (int p = px0; p < px1; p++)
                line_buf[p] = val;
        }
        line_pos++;
        if (line_pos >= cfg_video_w) {
            capturing = false;
            emit_line(disp_row);
            advance_row();
        }
    } else {
        if (s < lvl_sync_th) {
            low_run++;
        } else {
            if (low_run > cfg_field_w) {
                if (since_vsync > vsync_debounce) {
                    disp_row = 0;
                    since_vsync = 0;
                    field_count++;
                }
            } else if (low_run >= cfg_hsync_lo && low_run <= cfg_hsync_hi) {
                capturing = true;
                line_pos = -cfg_backporch_w;
            }
            low_run = 0;
        }
    }
}

void FpvRxProcessor::advance_row() {
    if (++disp_row >= cfg_field_lines)
        disp_row = 0;
    if (since_vsync < 99999)
        since_vsync++;
}

void FpvRxProcessor::apply_cfg(int idx, int val) {
    switch (idx) {
        case 0:
            cfg_field_lines = val;
            break;
        case 1:
            cfg_gap_trim = val;
            break;
        case 2:
            cfg_sync_th_bias = val;
            break;
        case 3:
            cfg_black_pct = val;
            break;
        case 4:
            cfg_white_pct = val;
            break;
        case 5:
            cfg_field_w = val;
            break;
        case 6:
            cfg_video_w = val;
            break;
        case 7:
            cfg_backporch_w = val;
            break;
        case 8:
            cfg_hsync_lo = val;
            break;
        case 9:
            cfg_hsync_hi = val;
            break;
        case 10:
            scan_mode = (val != 0);
            burst_pos = 0;  // restart video capture cleanly when leaving scan
            scan_acc = 0;
            scan_cnt = 0;
            break;
        default:
            break;
    }
}

void FpvRxProcessor::decode_burst() {
    int32_t sumI = 0, sumQ = 0;
    for (size_t k = 0; k < burst_samples; k++) {
        sumI += (int8_t)burst[2 * k];
        sumQ += (int8_t)burst[2 * k + 1];
    }
    const int32_t meanI = sumI / (int32_t)burst_samples;
    const int32_t meanQ = sumQ / (int32_t)burst_samples;

    uint16_t hist[256] = {0};
    int total = 0;
    {
        int32_t prevI = 0, prevQ = 0, a = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
        int dc = 0;
        for (size_t k = 0; k < burst_samples; k++) {
            const int32_t I = (int8_t)burst[2 * k] - meanI;
            const int32_t Q = (int8_t)burst[2 * k + 1] - meanQ;
            const int32_t re = I * prevI + Q * prevQ;
            const int32_t im = Q * prevI - I * prevQ;
            prevI = I;
            prevQ = Q;
            t0 = t1;
            t1 = t2;
            t2 = t3;
            t3 = t4;
            t4 = fpv_atan2(__SSAT(im, 16), __SSAT(re, 16));
            a += (t0 + t1 + t2 + t3 + t4) / 5;
            if (++dc >= DECIM) {
                int b = (a / DECIM + 32768) >> 8;
                if (b < 0)
                    b = 0;
                else if (b > 255)
                    b = 255;
                hist[b]++;
                total++;
                a = 0;
                dc = 0;
            }
        }
    }
    if (total < 16)
        return;
    lvl_sync = pct_level(hist, total, 1);
    lvl_black = pct_level(hist, total, cfg_black_pct);
    lvl_white = pct_level(hist, total, cfg_white_pct);
    if (lvl_white - lvl_black < 1)
        lvl_white = lvl_black + 1;
    lvl_sync_th = (lvl_sync + lvl_black) / 2 + cfg_sync_th_bias * 256;
    peak_permille = ((uint32_t)(lvl_black & 0xFFFF) << 16) | (uint32_t)(lvl_sync & 0xFFFF);

    low_run = 0;
    capturing = false;
    line_pos = 0;

    {
        int32_t prevI = 0, prevQ = 0, a = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
        int dc = 0;
        for (size_t k = 0; k < burst_samples; k++) {
            const int32_t I = (int8_t)burst[2 * k] - meanI;
            const int32_t Q = (int8_t)burst[2 * k + 1] - meanQ;
            const int32_t re = I * prevI + Q * prevQ;
            const int32_t im = Q * prevI - I * prevQ;
            prevI = I;
            prevQ = Q;
            t0 = t1;
            t1 = t2;
            t2 = t3;
            t3 = t4;
            t4 = fpv_atan2(__SSAT(im, 16), __SSAT(re, 16));
            a += (t0 + t1 + t2 + t3 + t4) / 5;
            if (++dc >= DECIM) {
                process_sample(a / DECIM);
                a = 0;
                dc = 0;
            }
        }
    }
}

void FpvRxProcessor::execute(const buffer_c8_t& buffer) {
    if (!configured)
        return;

    if (scan_mode) {
        // Channel-power scan: report AVERAGE band power (a wide FPV carrier raises
        // the mean; noise does not) as ChannelStatistics. Max power would saturate
        // on every channel from noise peaks and never discriminate. No video decode
        // and NO baseband image swap -> the run_image() hard-fault path is never taken.
        const size_t n = buffer.count;
        uint64_t sum = 0;
        for (size_t i = 0; i < n; i++) {
            const int re = buffer.p[i].real();
            const int im = buffer.p[i].imag();
            sum += (uint32_t)(re * re + im * im);
        }
        scan_acc += sum;
        scan_cnt += n;
        if (scan_cnt >= 500000) {                                           // ~50 ms @ 10 MHz -> ~20 channel measurements/sec
            const float mean = (float)scan_acc / (float)scan_cnt;           // mean |IQ|^2 in c8^2 units
            const int32_t db = mag2_to_dbv_norm(mean * (1.0f / 32258.0f));  // 0 dB = c8 full scale (2*127^2)
            const ChannelStatisticsMessage msg{{db, scan_cnt}};
            shared_memory.application_queue.push(msg);
            scan_acc = 0;
            scan_cnt = 0;
        }
        return;
    }

    // CAPTURE: append raw c8 IQ to the burst (memcpy keeps up at 10 MHz).
    if (burst_pos < burst_samples) {
        const size_t n = buffer.count;
        const size_t room = burst_samples - burst_pos;
        const size_t take = n < room ? n : room;
        memcpy(&burst[burst_pos * 2], buffer.p, take * 2);
        burst_pos += take;
    }
    // Full burst -> decode offline -> restart capture.
    if (burst_pos >= burst_samples) {
        const uint32_t t0 = *dwt_cyccnt;
        decode_burst();
        int adv = (int)((*dwt_cyccnt - t0) / cyc_per_line) + cfg_gap_trim;
        if (adv < 0) adv = 0;
        for (int i = 0; i < adv; i++)
            advance_row();
        burst_pos = 0;
    }

    if (lpc43xx::m4::flag_saturation())
        lpc43xx::m4::clear_flag_saturation();
}

void FpvRxProcessor::on_message(const Message* const message) {
    switch (message->id) {
        case Message::ID::WFMConfigure:
            *scb_demcr |= (1u << 24);
            *dwt_ctrl |= 1u;
            burst_pos = 0;
            low_run = 0;
            capturing = false;
            line_pos = 0;
            disp_row = 0;
            since_vsync = 99999;
            lvl_sync = 0;
            lvl_black = 0;
            lvl_white = 1;
            lvl_sync_th = 0;
            field_count = 0;
            configured = true;
            break;

        case Message::ID::SpectrumStreamingConfig:
            on_spectrum_streaming(*reinterpret_cast<const SpectrumStreamingConfigMessage*>(message));
            break;

        case Message::ID::SSTVRXPhaseSlant: {
            const auto& m = *reinterpret_cast<const SSTVRXPhaseSlantMessage*>(message);
            apply_cfg(m.phase, m.slant);
            break;
        }

        default:
            break;
    }
}

void FpvRxProcessor::on_spectrum_streaming(const SpectrumStreamingConfigMessage& message) {
    if (message.mode == SpectrumStreamingConfigMessage::Mode::Running) {
        streaming = true;
        ChannelSpectrumConfigMessage config{&fifo};
        shared_memory.application_queue.push(config);
    } else {
        streaming = false;
        fifo.reset_in();
    }
}

int main() {
    EventDispatcher event_dispatcher{std::make_unique<FpvRxProcessor>()};
    event_dispatcher.run();
    return 0;
}
