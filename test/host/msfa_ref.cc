// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 cardputer-fm1 contributors.
//
// Wraps upstream msfa (reference/msfa, Apache-2.0) so the C test can drive it.
// Nothing here reimplements anything: it constructs the upstream objects, feeds one
// note, and renders 64-sample blocks through SynthUnit's own path (LFO + Dx7Note +
// FmCore), which is exactly what a stock DX7 voice goes through.
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "synth.h"
#include "sin.h"
#include "exp2.h"
#include "freqlut.h"
#include "lfo.h"
#include "patch.h"
#include "controllers.h"
#include "dx7note.h"

extern "C" void msfa_upstream_init(void)
{
    Sin::init();
    Exp2::init();
    Freqlut::init(44100.0);
    Lfo::init(44100.0);
    PitchEnv::init(44100.0);
}

// Render `blocks` blocks of 64 samples for one note, applying note-off at
// `noteoff_block` (or never, when negative). `out` receives blocks * N int32.
extern "C" void msfa_upstream_render(const unsigned char *patch155, int midinote, int velocity,
                                     int *out, int noteoff_block, int blocks, int pitchbend)
{
    char unpacked[156];
    // patch155 carries byte 155 = operator mask, which upstream does not know about;
    // UnpackPatch always sets 0x3f.
    std::memcpy(unpacked, patch155, 156);
    unpacked[155] = 0x3f;

    // Value-initialised on purpose. Upstream's Dx7Note::init() does not clear fb_buf_,
    // and Lfo::reset() does not set phase_; `new Dx7Note` (no parens, as SynthUnit does)
    // leaves them indeterminate, so a stack instance makes block 1 depend on garbage.
    // Zeroing gives the deterministic behaviour a fresh voice has on the device.
    Dx7Note note{};
    Lfo lfo{};
    Controllers ctrls;
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrls.values_[kControllerPitch] = pitchbend;

    lfo.reset(unpacked + 137);
    note.init(unpacked, midinote, velocity);
    lfo.keydown();

    for (int b = 0; b < blocks; b++) {
        if (b == noteoff_block)
            note.keyup();
        int32_t lfovalue = lfo.getsample();
        int32_t lfodelay = lfo.getdelay();
        int32_t buf[N];
        for (int i = 0; i < N; i++)
            buf[i] = 0;
        note.compute(buf, lfovalue, lfodelay, &ctrls);
        for (int i = 0; i < N; i++)
            out[b * N + i] = buf[i];
    }
}
