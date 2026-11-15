// Flash layout and record format for saved songs (host-testable part).
//
// The last 4 KB sector of the 2 MB program-card flash holds a small
// append-only journal with the last-used slot. Below it are 6 song slots of
// 28 KB (7 sectors) each. A slot is a 16-byte header followed by the Song.
// All writing happens on core 1; the firmware is built copy_to_ram so the
// audio interrupt on core 0 never touches flash while it is being written.
// SPDX-License-Identifier: MIT
#pragma once

#include "song.h"

#include <cstdint>
#include <cstring>

namespace trk {
namespace store {

constexpr uint32_t kSectorSize = 4096;
constexpr uint32_t kPageSize = 256;
constexpr int kSlots = 6;
constexpr uint32_t kSlotSectors = 7;
constexpr uint32_t kSlotSize = kSlotSectors * kSectorSize;      // 28672
constexpr uint32_t kHeaderSize = 16;
constexpr uint32_t kRecordSize = kHeaderSize + sizeof(Song);
constexpr uint32_t kRecordPages = (kRecordSize + kPageSize - 1) / kPageSize;
constexpr uint32_t kSongMagic = 0x4B525457u;   // 'WTRK'
constexpr uint32_t kSysMagic = 0x59535457u;    // 'WTSY'
constexpr uint16_t kFormatVersion = 1;
static_assert(kRecordPages * kPageSize <= kSlotSize, "song must fit a slot");

// Offsets from the start of flash, for a firmware built for flash_size bytes.
constexpr uint32_t sys_offset(uint32_t flash_size) { return flash_size - kSectorSize; }
constexpr uint32_t slot_offset(uint32_t flash_size, int slot) {
    return sys_offset(flash_size) - static_cast<uint32_t>(kSlots - slot) * kSlotSize;
}
constexpr uint32_t reserved_bytes() { return kSectorSize + kSlots * kSlotSize; }   // 176 KB

inline uint32_t crc32(const uint8_t* d, uint32_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
inline uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// 16-byte header: magic, version(2) size(2), crc32 of the song, ~crc.
inline void make_header(const Song& s, uint8_t h[kHeaderSize]) {
    const uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(&s), sizeof(Song)) ^ 0xFFFFFFFFu;
    put32(h, kSongMagic);
    h[4] = kFormatVersion & 0xFF; h[5] = kFormatVersion >> 8;
    h[6] = sizeof(Song) & 0xFF; h[7] = (sizeof(Song) >> 8) & 0xFF;
    put32(h + 8, crc);
    put32(h + 12, ~crc);
}

// Validate a slot image in memory-mapped flash (or a test buffer).
inline bool slot_valid(const uint8_t* rec) {
    if (get32(rec) != kSongMagic) return false;
    if ((rec[4] | (rec[5] << 8)) != kFormatVersion) return false;
    if ((rec[6] | (rec[7] << 8)) != static_cast<int>(sizeof(Song) & 0xFFFF)) return false;
    const uint32_t crc = get32(rec + 8);
    if (get32(rec + 12) != ~crc) return false;
    return (crc32(rec + kHeaderSize, sizeof(Song)) ^ 0xFFFFFFFFu) == crc;
}

inline bool slot_load(const uint8_t* rec, Song& out) {
    if (!slot_valid(rec)) return false;
    std::memcpy(&out, rec + kHeaderSize, sizeof(Song));
    song_sanitize(out);
    return true;
}

// Fill page k (0..kRecordPages-1) of the record for song s.
inline void record_page(const Song& s, const uint8_t header[kHeaderSize], uint32_t k, uint8_t page[kPageSize]) {
    std::memset(page, 0xFF, kPageSize);
    const uint8_t* body = reinterpret_cast<const uint8_t*>(&s);
    for (uint32_t i = 0; i < kPageSize; ++i) {
        const uint32_t at = k * kPageSize + i;
        if (at < kHeaderSize) page[i] = header[at];
        else if (at < kRecordSize) page[i] = body[at - kHeaderSize];
    }
}

// --- system journal: 16 pages of {magic, slot, ~slot} -------------------------
inline void sys_page(int slot, uint8_t page[kPageSize]) {
    std::memset(page, 0xFF, kPageSize);
    put32(page, kSysMagic);
    page[4] = static_cast<uint8_t>(slot);
    page[5] = static_cast<uint8_t>(~slot);
}
inline bool page_erased(const uint8_t* p) {
    for (uint32_t i = 0; i < kPageSize; ++i) if (p[i] != 0xFF) return false;
    return true;
}
// Last stored slot, or -1.
inline int sys_last_slot(const uint8_t* sector) {
    int slot = -1;
    for (uint32_t k = 0; k < kSectorSize / kPageSize; ++k) {
        const uint8_t* p = sector + k * kPageSize;
        if (page_erased(p)) break;
        if (get32(p) == kSysMagic && static_cast<uint8_t>(~p[4]) == p[5] && p[4] < kSlots) slot = p[4];
    }
    return slot;
}
// Next free page index, or 16 if the sector must be erased first.
inline int sys_next_page(const uint8_t* sector) {
    for (uint32_t k = 0; k < kSectorSize / kPageSize; ++k)
        if (page_erased(sector + k * kPageSize)) return static_cast<int>(k);
    return static_cast<int>(kSectorSize / kPageSize);
}

} // namespace store
} // namespace trk
