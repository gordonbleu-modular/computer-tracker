// Flash layout and record format for saved songs (host-testable part).
//
// The last 4 KB sector of the 2 MB program-card flash holds a small
// append-only journal with the last-used slot. Below it are the 6 song
// slots of firmware 0.1/0.2 (28 KB = 7 sectors each), which are only read
// (old songs are converted when loaded). Below those are the 6 current slots,
// spaced 52 KB (13 sectors) apart because firmware 0.3.0 wrote 52 KB songs
// there. Firmware 0.3.1 writes a 36 KB record (9 sectors) at the same place
// and still reads a 0.3.0 record it finds there. A slot is a 16-byte header
// followed by the Song.
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
constexpr uint32_t kSlotStrideSectors = 13;                      // 0.3.0 slot size, kept for its records
constexpr uint32_t kSlotSize = kSlotStrideSectors * kSectorSize; // 53248 (distance between slots)
constexpr uint32_t kV2SlotSectors = 7;
constexpr uint32_t kV2SlotSize = kV2SlotSectors * kSectorSize;  // 28672
constexpr uint32_t kHeaderSize = 16;
constexpr uint32_t kRecordSize = kHeaderSize + sizeof(Song);
constexpr uint32_t kRecordPages = (kRecordSize + kPageSize - 1) / kPageSize;
constexpr uint32_t kSlotSectors = (kRecordSize + kSectorSize - 1) / kSectorSize;   // 9 sectors written per save
static_assert(kSlotSectors == 9, "32,996-byte record");
constexpr uint32_t kSongMagic = 0x4B525457u;   // 'WTRK'
constexpr uint32_t kSysMagic = 0x59535457u;    // 'WTSY'
constexpr uint16_t kFormatVersion = 3;          // 0.3.1: 8 tracks, 4-byte cells
constexpr uint16_t kV030FormatVersion = 2;      // 0.3.0: 8 tracks, 6-byte cells (two effects)
constexpr uint16_t kV2FormatVersion = 1;        // 0.1/0.2: 6 tracks, 4-byte cells
static_assert(kRecordPages * kPageSize <= kSlotSize, "song must fit a slot");
static_assert(kHeaderSize + kV2SongBytes <= kV2SlotSize, "v2 layout");
static_assert(kHeaderSize + kV030SongBytes <= kSlotSize, "0.3.0 layout");

// Offsets from the start of flash, for a firmware built for flash_size bytes.
constexpr uint32_t sys_offset(uint32_t flash_size) { return flash_size - kSectorSize; }
constexpr uint32_t v2_slot_offset(uint32_t flash_size, int slot) {
    return sys_offset(flash_size) - static_cast<uint32_t>(kSlots - slot) * kV2SlotSize;
}
constexpr uint32_t slot_offset(uint32_t flash_size, int slot) {
    return v2_slot_offset(flash_size, 0) - static_cast<uint32_t>(kSlots - slot) * kSlotSize;
}
constexpr uint32_t reserved_bytes() { return kSectorSize + kSlots * (kV2SlotSize + kSlotSize); }   // 484 KB

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

inline bool record_valid(const uint8_t* rec, uint16_t version, uint32_t size) {
    if (get32(rec) != kSongMagic) return false;
    if ((rec[4] | (rec[5] << 8)) != version) return false;
    if (static_cast<uint32_t>(rec[6] | (rec[7] << 8)) != (size & 0xFFFF)) return false;
    const uint32_t crc = get32(rec + 8);
    if (get32(rec + 12) != ~crc) return false;
    return (crc32(rec + kHeaderSize, size) ^ 0xFFFFFFFFu) == crc;
}

// Validate a current slot image in memory-mapped flash (or a test buffer).
inline bool slot_valid(const uint8_t* rec) { return record_valid(rec, kFormatVersion, sizeof(Song)); }
// Validate a slot written by firmware 0.3.0 (same address, two effect columns).
inline bool slot_valid_v030(const uint8_t* rec) { return record_valid(rec, kV030FormatVersion, kV030SongBytes); }
// Validate a slot written by firmware v0.1/v0.2.
inline bool slot_valid_v2(const uint8_t* rec) { return record_valid(rec, kV2FormatVersion, kV2SongBytes); }

inline bool slot_load(const uint8_t* rec, Song& out) {
    if (!slot_valid(rec)) return false;
    std::memcpy(&out, rec + kHeaderSize, sizeof(Song));
    song_sanitize(out);
    return true;
}

// Load slot k: the current record if valid, else convert a 0.3.0 record at
// the same address, else the old 0.1/0.2 record.
inline bool slot_load_any(const uint8_t* rec, const uint8_t* v2_rec, Song& out) {
    if (slot_load(rec, out)) return true;
    if (slot_valid_v030(rec)) {
        song_from_v030(rec + kHeaderSize, out);
        return true;
    }
    if (!slot_valid_v2(v2_rec)) return false;
    song_from_v2(v2_rec + kHeaderSize, out);
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
