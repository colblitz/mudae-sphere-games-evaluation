/**
 * board_io.h — lzma-compressed board file loading for all four games.
 *
 * Board file formats
 * ------------------
 * oc  (16,800 boards): raw ASCII, 25 bytes per board, no delimiters.
 *     Each byte encodes a color with a single character:
 *       R=spR O=spO Y=spY G=spG T=spT B=spB
 *
 * oq  (12,650 boards): raw little-endian uint32, one bitmask per board.
 *     Bit i is set iff cell i contains a purple sphere.
 *
 * ot  (varies by n_rare): raw little-endian int32 arrays, (3+n_rare) per board.
 *     Layout: [teal_mask, green_mask, yellow_mask, spo_mask, var_rare_0, ...]
 *     File name encodes n_rare: ot_boards_2.bin.lzma → n_rare=2 (6-color).
 *
 * All files are lzma-compressed (XZ/LZMA2).
 *
 * --text flag (generate_boards)
 * ------------------------------
 * If boards were generated with --text, the file is a plain text file
 * (one 25-char line per board for oc/oq; one line of space-separated hex
 * bitmasks for ot).  These are NOT committed to the repo.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lzma.h>

namespace sphere {

// ---------------------------------------------------------------------------
// lzma decompression helper
// ---------------------------------------------------------------------------

static inline std::vector<uint8_t> lzma_decompress(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "board_io: cannot open %s\n", path.c_str());
        return {};
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> compressed(static_cast<size_t>(fsz));
    if (fread(compressed.data(), 1, static_cast<size_t>(fsz), f) != static_cast<size_t>(fsz)) {
        fclose(f);
        fprintf(stderr, "board_io: read error on %s\n", path.c_str());
        return {};
    }
    fclose(f);

    // Decompress with a growing output buffer
    std::vector<uint8_t> out;
    out.resize(compressed.size() * 8);
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    if (ret != LZMA_OK) {
        fprintf(stderr, "board_io: lzma_stream_decoder error %d\n", (int)ret);
        return {};
    }
    strm.next_in   = compressed.data();
    strm.avail_in  = compressed.size();
    size_t out_pos = 0;
    for (;;) {
        strm.next_out  = out.data() + out_pos;
        strm.avail_out = out.size() - out_pos;
        ret = lzma_code(&strm, LZMA_FINISH);
        out_pos = out.size() - strm.avail_out;
        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK) {
            lzma_end(&strm);
            fprintf(stderr, "board_io: decompression error %d after %zu bytes\n",
                    (int)ret, out_pos);
            return {};
        }
        if (strm.avail_out == 0) out.resize(out.size() * 2);
    }
    lzma_end(&strm);
    out.resize(out_pos);
    return out;
}

// ---------------------------------------------------------------------------
// oc boards: 25 bytes per board (ASCII color chars)
// ---------------------------------------------------------------------------

// Single oc board: array of 25 color ints
// 0=spR 1=spO 2=spY 3=spG 4=spT 5=spB
struct OCBoard {
    uint8_t cells[25];  // color int per cell
};

static constexpr uint8_t OC_COLOR_COUNT = 6;
static const char* OC_COLOR_NAMES[OC_COLOR_COUNT] = {
    "spR", "spO", "spY", "spG", "spT", "spB"
};
static constexpr int OC_COLOR_VALUES[OC_COLOR_COUNT] = { 150, 90, 55, 35, 20, 10 };

static inline uint8_t oc_char_to_color(char c) {
    switch (c) {
        case 'R': return 0;
        case 'O': return 1;
        case 'Y': return 2;
        case 'G': return 3;
        case 'T': return 4;
        case 'B': return 5;
        default:  return 5;  // treat unknown as blue
    }
}

static inline std::vector<OCBoard> load_oc_boards(const std::string& path) {
    auto raw = lzma_decompress(path);
    if (raw.empty()) return {};
    const size_t n = raw.size() / 25;
    std::vector<OCBoard> boards(n);
    for (size_t i = 0; i < n; ++i) {
        for (int j = 0; j < 25; ++j)
            boards[i].cells[j] = oc_char_to_color(static_cast<char>(raw[i * 25 + j]));
    }
    return boards;
}

// ---------------------------------------------------------------------------
// oq boards: uint32 bitmask per board (bit i = cell i is purple)
// ---------------------------------------------------------------------------

static inline std::vector<uint32_t> load_oq_boards(const std::string& path) {
    auto raw = lzma_decompress(path);
    if (raw.empty()) return {};
    const size_t n = raw.size() / 4;
    std::vector<uint32_t> masks(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        memcpy(&v, raw.data() + i * 4, 4);
        masks[i] = v;
    }
    return masks;
}

// ---------------------------------------------------------------------------
// ot boards: (3 + n_rare) int32 bitmasks per board
// ---------------------------------------------------------------------------

struct OTBoard {
    int32_t teal;
    int32_t green;
    int32_t yellow;
    int32_t spo;
    int32_t var_rare[4];  // up to 4 variable rare ships
    int     n_var_rare;   // number of variable rare ships (n_rare - 1)
};

static inline std::vector<OTBoard> load_ot_boards(const std::string& path, int n_rare) {
    auto raw = lzma_decompress(path);
    if (raw.empty()) return {};
    const int   fields      = 3 + n_rare;  // teal+green+yellow+spo+var_rare...
    const size_t board_size = static_cast<size_t>(fields) * 4;
    const size_t n          = raw.size() / board_size;
    int n_var_rare = n_rare - 1;
    std::vector<OTBoard> boards(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = raw.data() + i * board_size;
        auto ri = [&](int k) -> int32_t {
            int32_t v = 0;
            memcpy(&v, p + k * 4, 4);
            return v;
        };
        boards[i].teal      = ri(0);
        boards[i].green     = ri(1);
        boards[i].yellow    = ri(2);
        boards[i].spo       = ri(3);
        boards[i].n_var_rare = n_var_rare;
        for (int k = 0; k < n_var_rare; ++k)
            boards[i].var_rare[k] = ri(4 + k);
    }
    return boards;
}

// Derive the color of each cell from an OTBoard.
// Returns array of 25 color strings.
// Ship colors by ship index: 0=teal, 1=green, 2=yellow, 3=spo(orange), 4+=var_rare
// var_rare color assignment (by slot index):
//   6-color (n_rare=2): slot 0 → spL
//   7-color (n_rare=3): slot 0 → spL, slot 1 → spD
//   8-color (n_rare=4): slot 0 → spL, slot 1 → spD, slot 2 → spR
//   9-color (n_rare=5): slot 0 → spL, slot 1 → spD, slot 2 → spR, slot 3 → spW
static const char* OT_VAR_RARE_COLORS[] = { "spL", "spD", "spR", "spW" };

static inline std::vector<std::string> ot_board_colors(const OTBoard& b) {
    std::vector<std::string> colors(25, "spB");
    auto paint = [&](int32_t mask, const char* color) {
        for (int i = 0; i < 25; ++i)
            if ((mask >> i) & 1) colors[i] = color;
    };
    paint(b.teal,   "spT");
    paint(b.green,  "spG");
    paint(b.yellow, "spY");
    paint(b.spo,    "spO");
    for (int k = 0; k < b.n_var_rare; ++k)
        paint(b.var_rare[k], OT_VAR_RARE_COLORS[k]);
    return colors;
}

}  // namespace sphere
