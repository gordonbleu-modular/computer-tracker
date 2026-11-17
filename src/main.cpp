// Workshop Tracker — a 6-track tracker sequencer with 4 drum-synth voices for
// the Music Thing Modular Workshop System Computer, edited live from a web
// page over USB MIDI SysEx and playable standalone.
//
// Core 0: ComputerCard audio interrupt -> TrackerEngine::process() (48 kHz).
// Core 1: TinyUSB device (MIDI), SysEx protocol, song load/save to flash.
// The image is built copy_to_ram, so core 0 never touches flash and core 1
// can erase/program it without stopping the audio.
// SPDX-License-Identifier: MIT

#include "ComputerCard.h"

#include "hardware/flash.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "engine/engine.h"
#include "engine/flashstore.h"
#include "engine/protocol.h"

#include <cstring>

namespace {

constexpr uint32_t kSysClockKhz = 192000;               // 4 x 48 MHz
constexpr uint32_t kCyclesPerSample = kSysClockKhz * 1000 / trk::kSampleRate;   // 4000
constexpr uint32_t kFlashSize = PICO_FLASH_SIZE_BYTES;

trk::Song g_song_a;
trk::Song g_song_b;
trk::TrackerEngine g_engine;

// CPU load of ProcessSample, written by core 0, read/reset by core 1.
volatile uint32_t g_cyc_sum = 0, g_cyc_n = 0, g_cyc_max = 0;

const uint8_t* flash_ptr(uint32_t offset) {
    return reinterpret_cast<const uint8_t*>(XIP_BASE + offset);
}

int boot_slot() {
    const int s = trk::store::sys_last_slot(flash_ptr(trk::store::sys_offset(kFlashSize)));
    return s < 0 ? 0 : s;
}

class TrackerCard : public ComputerCard {
public:
    void __not_in_flash_func(ProcessSample)() override {
        const uint32_t t0 = systick_hw->cvr;

        trk::EngineIn in;
        in.audio[0] = AudioIn1();
        in.audio[1] = AudioIn2();
        in.cv[0] = CVIn1();
        in.cv[1] = CVIn2();
        in.cv_connected[0] = Connected(Input::CV1);
        in.cv_connected[1] = Connected(Input::CV2);
        in.audio_connected[0] = Connected(Input::Audio1);
        in.audio_connected[1] = Connected(Input::Audio2);
        in.clock_connected = Connected(Input::Pulse1);
        in.clock_rise = PulseIn1RisingEdge();
        in.reset_rise = PulseIn2RisingEdge();
        in.knobs[0] = KnobVal(Knob::Main);
        in.knobs[1] = KnobVal(Knob::X);
        in.knobs[2] = KnobVal(Knob::Y);
        const Switch sw = SwitchVal();
        in.sw = sw == Switch::Up ? trk::Sw::Up : (sw == Switch::Down ? trk::Sw::Down : trk::Sw::Middle);

        trk::EngineOut out;
        g_engine.process(in, out);

        AudioOut1(out.audio[0]);
        AudioOut2(out.audio[1]);
        CVOut1Millivolts(out.cv_mv[0]);
        CVOut2Millivolts(out.cv_mv[1]);
        PulseOut1(out.gate[0]);
        PulseOut2(out.gate[1]);
        if (out.leds_valid)
            for (int i = 0; i < 6; ++i) LedBrightness(static_cast<uint32_t>(i), out.led[i]);

        const uint32_t cyc = (t0 - systick_hw->cvr) & 0x00FFFFFFu;
        g_cyc_sum = g_cyc_sum + cyc;
        g_cyc_n = g_cyc_n + 1;
        if (cyc > g_cyc_max) g_cyc_max = cyc;
    }
};

TrackerCard g_card;

// ---------------------------------------------------------------------------
// Core 1: USB MIDI + protocol + flash
// ---------------------------------------------------------------------------

void midi_write_blocking(const uint8_t* data, uint32_t n) {
    uint32_t sent = 0;
    uint32_t spins = 0;
    while (sent < n && tud_mounted()) {
        const uint32_t k = tud_midi_stream_write(0, data + sent, n - sent);
        sent += k;
        if (!k) {
            tud_task();
            if (++spins > 200000) return;   // host stopped reading: give up
        }
    }
}

class FirmwareHost : public trk::proto::Host {
public:
    trk::Song* live() override { return g_engine.live(); }

    void send(const uint8_t* body, int len) override {
        if (!tud_mounted()) return;
        const uint8_t head[3] = {0xF0, trk::proto::kMfr, trk::proto::kDevice};
        const uint8_t tail[1] = {0xF7};
        midi_write_blocking(head, 3);
        midi_write_blocking(body, static_cast<uint32_t>(len));
        midi_write_blocking(tail, 1);
    }

    bool command(const trk::Command& c) override { return g_engine.commands().push(c); }

    void request_save(int slot) override { job_ = Job::Save; job_slot_ = slot; }
    void request_load(int slot) override { job_ = Job::Load; job_slot_ = slot; }
    void request_init(int kind) override { job_ = Job::Init; job_slot_ = kind; }

    void info_fields(uint8_t& slot, uint8_t& mask) override {
        slot = static_cast<uint8_t>(g_engine.status().slot);
        mask = 0;
        for (int i = 0; i < trk::store::kSlots; ++i)
            if (trk::store::slot_valid(flash_ptr(trk::store::slot_offset(kFlashSize, i)))) mask |= static_cast<uint8_t>(1u << i);
    }

    void pinged() override {
        const uint32_t now = time_us_32();
        if (!pinged_once_ || now - last_ping_ > 3000000u) reporter_.force();   // editor came back
        last_ping_ = now;
        pinged_once_ = true;
    }
    void hello() override { reporter_.force(); }

    trk::proto::PanelReporter reporter_;

    enum class Job { None, Save, Load, Init };
    Job job_ = Job::None;
    int job_slot_ = 0;
    uint32_t last_ping_ = 0;
    bool pinged_once_ = false;
};

FirmwareHost g_host;
trk::proto::Protocol g_protocol(g_host);
uint32_t g_swaps_requested = 0;
uint8_t g_page[trk::store::kPageSize];
uint8_t g_rx[64];

void service_usb() {
    tud_task();
    while (tud_midi_available()) {
        const uint32_t n = tud_midi_stream_read(g_rx, sizeof(g_rx));
        if (!n) break;
        g_protocol.feed(g_rx, static_cast<int>(n));
    }
}

// Wait until core 0 has performed every swap we asked for.
void wait_swap_idle() {
    while (g_engine.status().swap_count != g_swaps_requested || !g_engine.swap_idle()) service_usb();
}

void request_swap(bool immediate) {
    ++g_swaps_requested;
    while (!g_engine.commands().push(trk::Command{trk::CmdType::SwapSong, static_cast<uint8_t>(immediate ? 1 : 0), 0, 0})) service_usb();
}

bool __not_in_flash_func(write_slot)(int slot, const trk::Song& s) {
    using namespace trk::store;
    const uint32_t base = slot_offset(kFlashSize, slot);
    uint8_t header[kHeaderSize];
    make_header(s, header);
    for (uint32_t k = 0; k < kSlotSectors; ++k) {
        const uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(base + k * kSectorSize, kSectorSize);
        restore_interrupts(ints);
        service_usb();
    }
    for (uint32_t k = 0; k < kRecordPages; ++k) {
        record_page(s, header, k, g_page);
        const uint32_t ints = save_and_disable_interrupts();
        flash_range_program(base + k * kPageSize, g_page, kPageSize);
        restore_interrupts(ints);
        if ((k & 15) == 15) service_usb();
    }
    return slot_valid(flash_ptr(base));
}

void __not_in_flash_func(write_sys_slot)(int slot) {
    using namespace trk::store;
    const uint32_t off = sys_offset(kFlashSize);
    if (sys_last_slot(flash_ptr(off)) == slot) return;
    int page = sys_next_page(flash_ptr(off));
    if (page >= static_cast<int>(kSectorSize / kPageSize)) {
        const uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(off, kSectorSize);
        restore_interrupts(ints);
        page = 0;
    }
    sys_page(slot, g_page);
    const uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off + static_cast<uint32_t>(page) * kPageSize, g_page, kPageSize);
    restore_interrupts(ints);
}

// Load slot (or a fresh song) into the spare buffer and swap it in.
// kind: 0..5 = slot, 100 = empty song, 101 = demo.
bool load_into_spare_and_swap(int kind, bool immediate) {
    wait_swap_idle();
    trk::Song* sp = g_engine.spare();
    bool ok = true;
    if (kind == 100) {
        trk::song_clear(*sp);
    } else if (kind == 101) {
        trk::song_demo(*sp);
    } else {
        ok = trk::store::slot_load(flash_ptr(trk::store::slot_offset(kFlashSize, kind)), *sp);
        if (!ok && kind == 0) { trk::song_demo(*sp); ok = true; }   // slot 1 falls back to the demo
    }
    if (!ok) return false;
    request_swap(immediate);
    if (kind < trk::store::kSlots) {
        g_engine.commands().push(trk::Command{trk::CmdType::SetSlot, static_cast<uint8_t>(kind), 0, 0});
    }
    return true;
}

void run_job() {
    using J = FirmwareHost::Job;
    const J job = g_host.job_;
    if (job == J::None) return;
    const int arg = g_host.job_slot_;
    g_host.job_ = J::None;
    switch (job) {
    case J::Save: {
        // Snapshot the live song into the spare buffer first so the CRC
        // matches what we write even if the panel changes the tempo meanwhile.
        wait_swap_idle();
        trk::Song* snap = g_engine.spare();
        std::memcpy(snap, g_engine.live(), sizeof(trk::Song));
        g_engine.set_busy(true);
        const bool ok = write_slot(arg, *snap);
        if (ok) write_sys_slot(arg);
        g_engine.set_busy(false);
        if (ok) g_engine.commands().push(trk::Command{trk::CmdType::SetSlot, static_cast<uint8_t>(arg), 0, 0});
        g_protocol.send_ack(trk::proto::Save, ok ? trk::proto::AckOk : trk::proto::AckError, static_cast<uint8_t>(arg));
        break;
    }
    case J::Load: {
        const bool ok = load_into_spare_and_swap(arg, true);
        if (ok) { wait_swap_idle(); write_sys_slot(arg); }
        g_protocol.send_ack(trk::proto::Load, ok ? trk::proto::AckOk : trk::proto::AckEmpty, static_cast<uint8_t>(arg));
        break;
    }
    case J::Init: {
        load_into_spare_and_swap(arg == 1 ? 101 : 100, true);
        wait_swap_idle();
        g_protocol.send_ack(trk::proto::InitSong, trk::proto::AckOk, static_cast<uint8_t>(arg));
        break;
    }
    case J::None:
        break;
    }
}

void send_status_if_due() {
    static uint32_t last = 0;
    static uint32_t last_trig[trk::kDrumTracks] = {};
    const uint32_t now = time_us_32();
    if (!g_host.pinged_once_ || now - g_host.last_ping_ > 3000000u) return;
    if (now - last < 50000u) return;
    last = now;
    const uint32_t n = g_cyc_n, sum = g_cyc_sum, mx = g_cyc_max;
    g_cyc_n = 0; g_cyc_sum = 0; g_cyc_max = 0;
    const uint32_t avg = n ? sum / n : 0;
    const uint8_t cpu_now = static_cast<uint8_t>(trk::clampi(static_cast<int>(avg * 100 / kCyclesPerSample), 0, 127));
    const uint8_t cpu_peak = static_cast<uint8_t>(trk::clampi(static_cast<int>(mx * 100 / kCyclesPerSample), 0, 127));
    uint8_t trig = 0;
    for (int i = 0; i < trk::kDrumTracks; ++i) {
        const uint32_t c = g_engine.status().trig_count[i];
        if (c != last_trig[i]) trig |= static_cast<uint8_t>(1u << i);
        last_trig[i] = c;
    }
    uint8_t buf[64];
    const int len = trk::proto::build_status(buf, g_engine.status(), *g_engine.live(), g_engine.mixer(), cpu_now, cpu_peak, trig);
    g_host.send(buf, len);
}

// Knob/macro feedback for the editor (only while it pings; on change, <= ~30/s).
void send_panel_if_due() {
    const uint32_t now = time_us_32();
    if (!g_host.pinged_once_ || now - g_host.last_ping_ > 3000000u) return;
    uint8_t buf[2 + trk::proto::kPanelBytes];
    const int len = g_host.reporter_.poll(now, g_engine.status(), *g_engine.live(), buf);
    if (len > 0) g_host.send(buf, len);
}

void core1_main() {
    tusb_init();
    while (true) {
        service_usb();
        run_job();
        // Song slot chosen on the panel: load at the end of the pattern.
        const int s = g_engine.take_slot_request();
        if (s >= 0) {
            if (load_into_spare_and_swap(s, false)) {
                // Remember it once the swap has happened (handled lazily).
            }
        }
        send_status_if_due();
        send_panel_if_due();
    }
}

} // namespace

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(kSysClockKhz, true);

    // SysTick counts core-0 CPU cycles for the load meter.
    systick_hw->rvr = 0x00FFFFFFu;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x5;   // enable, processor clock

    // Load the last-used slot (falls back to the demo song).
    const int slot = boot_slot();
    if (!trk::store::slot_load(flash_ptr(trk::store::slot_offset(kFlashSize, slot)), g_song_a)) {
        trk::song_demo(g_song_a);
    }
    trk::song_clear(g_song_b);

    trk::EngineIn in;
    g_engine.init(&g_song_a, &g_song_b, slot, in);
    g_engine.set_warmup(trk::kSampleRate / 10);

    g_card.EnableNormalisationProbe();
    multicore_launch_core1(core1_main);
    g_card.Run();
}
