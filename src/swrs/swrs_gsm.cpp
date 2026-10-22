// GSM 06.10 full-rate speech decoding for Rogue Squadron's voice samples, replacing the game's
// hand-written assembly module at 0x800918A0-0x80091E9F (N64Recomp can't translate it: it finds its
// tables with `bltzal $zero` and calls helpers through `bal`/`jalr`).
//
// Written from the GSM 06.10 standard (sections cited below). The game's decoder differs from the
// standard's reference arithmetic in ways that change the output, so those are reproduced:
// - Multiplications with rounding keep the low 32 bits of the product before adding 0x4000 and
//   shifting right by 15 (no 16-bit saturation), and sums aren't saturated.
// - The short-term filter state v[0..7] is kept as 32-bit words, and the de-emphasis memory msr stays
//   32-bit within one call (stored as a halfword at the end).
// The game's entry points, arguments and state layout are listed with each function.

#include <cstdint>

#include "recomp.h"

namespace {

// Rounded fixed-point product as the game computes it (see the top of this file).
int32_t mult_round(int32_t a, int32_t b) {
    const uint32_t low = uint32_t(int64_t(a) * int64_t(b));
    return int32_t(low + 0x4000u) >> 15;
}

gpr address(uint32_t a) {
    return gpr(int32_t(a));
}

// 4.2.15: exponent and mantissa of the block maximum xmaxc.
void xmax_exponent_mantissa(int32_t xmaxc, int32_t& exponent, int32_t& mantissa) {
    exponent = 0;
    if (xmaxc > 15) {
        exponent = (xmaxc >> 3) - 1;
    }

    mantissa = xmaxc - (exponent << 3);
    if (mantissa == 0) {
        exponent = -4;
        mantissa = 7;
        return;
    }

    while (mantissa <= 7) {
        mantissa = (mantissa << 1) | 1;
        exponent--;
    }

    mantissa -= 8;
}

// Table 4.3a: quantization levels of the long-term predictor gain.
constexpr int32_t LongTermGains[4] = { 3277, 11469, 21299, 32767 };

// 4.2.9.2: LAR to reflection coefficient, as a 16-bit value.
int32_t lar_to_reflection(int32_t lar) {
    const bool negative = lar < 0;
    int32_t magnitude = negative ? -lar : lar;
    if (magnitude < 11059) {
        magnitude <<= 1;
    }
    else if (magnitude < 20070) {
        magnitude += 11059;
    }
    else {
        magnitude = (magnitude >> 2) + 26112;
    }

    if (magnitude > 32767) {
        magnitude = 32767;
    }

    return int16_t(negative ? -magnitude : magnitude);
}

// 4.2.9.1: LAR interpolation between the previous (j-1) and current (j) frame's decoded LARs, for the four
// sample ranges 0-12, 13-26, 27-39 and 40-159.
int32_t interpolate_lar(int range, int32_t current, int32_t previous) {
    switch (range) {
    case 0:
        return (previous >> 2) + (current >> 2) + (previous >> 1);
    case 1:
        return (previous >> 1) + (current >> 1);
    case 2:
        return (previous >> 2) + (current >> 2) + (current >> 1);
    default:
        return current;
    }
}

struct ShortTermState {
    int32_t v[8];
    int32_t msr;
};

// 4.3.4 short-term synthesis lattice filter, then 4.3.5 de-emphasis (28180 = 0.86) and 4.3.6/4.3.7
// upscaling and truncation to 13 bits. Reads `count` halfwords of the residual and writes as many samples.
void short_term_synthesis(uint8_t* rdram, ShortTermState& state, const int32_t rp[8], uint32_t residual, uint32_t output, int count) {
    int32_t* v = state.v;
    for (int k = 0; k < count; k++) {
        int32_t sr = MEM_H(k * 2, address(residual));
        sr -= mult_round(v[7], rp[7]);
        for (int i = 6; i >= 0; i--) {
            sr -= mult_round(v[i], rp[i]);
            v[i + 1] = v[i] + mult_round(rp[i], sr);
        }

        v[0] = sr;

        state.msr = sr + mult_round(28180, state.msr);

        // Doubling with saturation to 16 bits, then the three low bits are cleared.
        int32_t upscaled = int32_t(uint32_t(state.msr) << 1);
        const int32_t overflow = ((upscaled >> 15) + 1) >> 1;
        if (overflow > 0) {
            upscaled = 32767;
        }
        else if (overflow < 0) {
            upscaled = -32768;
        }

        MEM_H(k * 2, address(output)) = int16_t(uint32_t(upscaled) & 0xFFF8);
    }
}

} // namespace

// 4.3.1 APCM inverse quantization and 4.3.2 RPE grid positioning (game function 0x800918A0).
// a0 = xmaxc, a1 = Mc, a2 = xMc[13] (halfwords, replaced by the dequantized xMp), a3 = erp[40] (halfwords).
extern "C" void func_800918A0(uint8_t* rdram, recomp_context* ctx) {
    const int32_t xmaxc = int32_t(ctx->r4);
    const int32_t mc = int32_t(ctx->r5);
    const uint32_t samples = uint32_t(ctx->r6);
    const uint32_t erp = uint32_t(ctx->r7);

    int32_t exponent, mantissa;
    xmax_exponent_mantissa(xmaxc, exponent, mantissa);

    // FAC[mantissa] (table 4.5) and the rounding term for the final shift. With a shift of 0 the game adds
    // 1 << 31, which leaves the 16-bit result unchanged.
    const int32_t factor = 18431 + (mantissa << 11);
    const int32_t shift = 6 - exponent;
    const int32_t rounding = int32_t(1u << ((shift - 1) & 31));
    for (int i = 0; i < 13; i++) {
        int32_t value = MEM_H(i * 2, address(samples));
        value = int32_t(uint32_t(value) << 13) - 0x7000;
        value = mult_round(value, factor) + rounding;
        MEM_H(i * 2, address(samples)) = int16_t(value >> (shift & 31));
    }

    for (int i = 0; i < 40; i++) {
        MEM_H(i * 2, address(erp)) = 0;
    }

    for (int i = 0; i < 13; i++) {
        MEM_H((mc + i * 3) * 2, address(erp)) = MEM_H(i * 2, address(samples));
    }
}

// 4.3.3 long-term synthesis filtering (game function 0x80091994).
// a0 = state: +0x00 the 120 previous reconstructed samples, +0xF0 the 40 new ones, +0x166 nrp.
// a1 = Ncr, a2 = bcr, a3 = erp[40].
extern "C" void func_80091994(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t state = uint32_t(ctx->r4);
    const int32_t ncr = int32_t(ctx->r5);
    const int32_t bcr = int32_t(ctx->r6);
    const uint32_t erp = uint32_t(ctx->r7);

    // A lag outside 40-120 keeps the previous one.
    int32_t lag = MEM_H(0x166, address(state));
    if ((ncr >= 40) && (ncr <= 120)) {
        lag = ncr;
    }

    MEM_H(0x166, address(state)) = int16_t(lag);

    const int32_t gain = LongTermGains[bcr & 3];

    // The last 120 samples (including the previous frame's 40) become the history.
    for (int i = 0; i < 120; i++) {
        MEM_H(i * 2, address(state)) = MEM_H(0x50 + i * 2, address(state));
    }

    for (int k = 0; k < 40; k++) {
        const int32_t delayed = MEM_H(0xF0 + (k - lag) * 2, address(state));
        MEM_H(0xF0 + k * 2, address(state)) = int16_t(mult_round(delayed, gain) + MEM_H(k * 2, address(erp)));
    }
}

// 4.3.4-4.3.7 short-term synthesis and post-processing of one frame (game function 0x80091C14).
// a0 = state: +0x140 v[0..7] (words), +0x164 msr, +0x168 and +0x178 the decoded LARs of two frames (8
// halfwords each), +0x188 nonzero when the one at +0x178 is the current frame's. a1 = wrp[160] (residual),
// a2 = sr[160] (output samples).
extern "C" void func_80091C14(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t state = uint32_t(ctx->r4);
    const uint32_t residual = uint32_t(ctx->r5);
    const uint32_t output = uint32_t(ctx->r6);

    ShortTermState filter;
    for (int i = 0; i < 8; i++) {
        filter.v[i] = MEM_W(0x140 + i * 4, address(state));
    }

    filter.msr = MEM_H(0x164, address(state));

    uint32_t current = state + 0x168;
    uint32_t previous = state + 0x178;
    if (MEM_H(0x188, address(state)) != 0) {
        current = state + 0x178;
        previous = state + 0x168;
    }

    static const int firstSample[4] = { 0, 13, 27, 40 };
    static const int sampleCount[4] = { 13, 14, 13, 120 };
    for (int range = 0; range < 4; range++) {
        int32_t rp[8];
        for (int i = 0; i < 8; i++) {
            rp[i] = lar_to_reflection(interpolate_lar(range, MEM_H(i * 2, address(current)), MEM_H(i * 2, address(previous))));
        }

        const uint32_t offset = uint32_t(firstSample[range] * 2);
        short_term_synthesis(rdram, filter, rp, residual + offset, output + offset, sampleCount[range]);
    }

    for (int i = 0; i < 8; i++) {
        MEM_W(0x140 + i * 4, address(state)) = filter.v[i];
    }

    MEM_H(0x164, address(state)) = int16_t(filter.msr);
}
