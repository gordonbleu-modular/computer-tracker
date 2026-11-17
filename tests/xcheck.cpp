// Cross-language check helper for web/test/xcheck.test.mjs.
//   xcheck demo   -> raw bytes of song_demo() (header + cells) on stdout
//   xcheck empty  -> raw bytes of song_clear()
//   xcheck apply  -> reads a SysEx stream on stdin, applies it to an empty
//                    song through proto::Protocol, then answers GetSong and
//                    writes the framed reply on stdout.
//   xcheck panel  -> reads a SysEx stream (song edits) like apply, then writes
//                    one framed Panel report for a fixed engine status
//                    (see web/test/xcheck.test.mjs for the values).
// SPDX-License-Identifier: MIT
#include "engine/protocol.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace trk;

struct PipeHost : proto::Host {
    Song* song;
    explicit PipeHost(Song* s) : song(s) {}
    Song* live() override { return song; }
    void send(const uint8_t* b, int n) override {
        std::fputc(0xF0, stdout); std::fputc(proto::kMfr, stdout); std::fputc(proto::kDevice, stdout);
        std::fwrite(b, 1, static_cast<size_t>(n), stdout);
        std::fputc(0xF7, stdout);
    }
    bool command(const Command&) override { return true; }
    void request_save(int) override {}
    void request_load(int) override {}
    void request_init(int) override {}
    void info_fields(uint8_t& slot, uint8_t& mask) override { slot = 0; mask = 0; }
    void pinged() override {}
};

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    auto s = std::make_unique<Song>();
    if (!std::strcmp(argv[1], "demo") || !std::strcmp(argv[1], "empty")) {
        if (argv[1][0] == 'd') song_demo(*s); else song_clear(*s);
        std::fwrite(s.get(), 1, sizeof(Song), stdout);
        return 0;
    }
    const bool panel = !std::strcmp(argv[1], "panel");
    if (std::strcmp(argv[1], "apply") && !panel) return 2;
    song_clear(*s);
    PipeHost h(s.get());
    proto::Protocol pr(h);
    std::vector<uint8_t> in;
    int c;
    while ((c = std::fgetc(stdin)) != EOF) in.push_back(static_cast<uint8_t>(c));
    pr.feed(in.data(), static_cast<int>(in.size()));
    if (panel) {
        EngineStatus st;
        st.flags = 16; st.down_held = 0;
        st.knob_waiting = 5;
        st.macro[0] = 25; st.macro[1] = -40; st.macro[2] = -64;
        st.slot = 4; st.mode = 2; st.entry = 0; st.pattern = 9; st.transpose = 12;
        const proto::PanelView v{{10, 64, 127}};
        uint8_t b[64];
        const int n = proto::build_panel(b, st, *s, v);
        h.send(b, n);
        return 0;
    }
    const uint8_t get[] = {0xF0, proto::kMfr, proto::kDevice, proto::GetSong, 0xF7};
    pr.feed(get, sizeof get);
    return 0;
}
