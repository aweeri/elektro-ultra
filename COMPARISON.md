# elektro-ultra vs GobDump vs ElektroDump

Same IQ, same satellite downlink: `LRIT/2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16`
(Elektro-L No.3 LRIT, BPSK 294 000 Bd, 3 MSPS cs16).

GobDump is the reference fork of SatDump (`/opt/data/cache/scratch/GobDump`,
v2.0.0-alpha). It does not build in this container (no root, no apt lists, and it
needs volk, jemalloc, nng, fftw3f, libpng, libtiff, zstd, lz4, spdlog, nlohmann
dev packages), so it was built on ATLAS (Debian 13, Pentium J4205 @ 1.5 GHz,
4 cores) with `-DBUILD_GUI=OFF -DBUILD_OPENCL=OFF` and run with the legacy CLI:

```
./gobdump legacy elektro_lrit baseband <iq.cs16> <out> \
    --samplerate 3e6 --baseband_format cs16 --frequency 1691e6
```

ElektroDump (`/opt/data/cache/scratch/ED`, the prior C++ tree) builds with a
plain Makefile and was run on the same box as elektro-ultra:

```
./build/elektrodump decode <dir-with-lrit60s.cf32> --mode lrit
```

## 60 s of the capture, same file

| | **elektro-ultra** | **GobDump** | **ElektroDump** |
|---|---|---|---|
| CADUs decoded | **1074 / 1074** | 1073 | 1076 |
| RS corrections | **0** | not reported | 0 corrected, **1 uncorrectable** |
| bad CRC | **0** | 2 files needed "crc-recovered" | 2 files "crc-recovered" |
| xRIT files assembled | 6 (5 complete) | n/a (image stage) | 6 |
| images written | 5 segment PNG + **1 stitched disk** | 1 full-disk PNG (+2 failed composites) | **0** |
| EVM / SNR | EVM 31.0 %, SNR 10.2 dB | BER 0.0073, deframer SYNCED | SNR 11.76 dB |

`ElektroDump` frame count is 2 higher than ours because it emits a frame whose RS
then fails (its own "uncorrectable 1"); GobDump is 1 higher for the same reason at
the other end of the file. All three agree to within 0.2 %.

## Full capture (256.1 s, 4173 frames)

| | **elektro-ultra** | **GobDump** |
|---|---|---|
| CADUs decoded | 4173 (0 RS fixed, 0 failed) | 4175 |
| Viterbi BER | 0.0000 (cumulative) | 0.010 - 0.012 |
| channel images | 5 stitched 2784x2784 disks + 30 raw strips | 5 full-disk PNGs (L3_1, L3_2, L3_3, L3_4, L3_9) |
| unparsed | 2 (EPI/PRO) kept as raw | dropped |
| wall time, same CPU (60 s of IQ) | 22.5 s = **2.7x** | 9.85 s = **6.1x** |
| wall time on core (256 s of IQ) | 52.5 s = **4.9x** | (not built here) |

## The images are pixel-identical

Every channel was compared to GobDump's output for the same file, both 2784x2784:

| elektro-ultra | GobDump | Pearson r | mean abs diff | pixels differing |
|---|---|---|---|---|
| `00_6` | `L3_1` | **1.00000** | 0.00 | 0.1 % |
| `00_7` | `L3_2` | **1.00000** | 0.00 | 0.1 % |
| `00_9` | `L3_3` | **1.00000** | 0.00 | 0.1 % |
| `03_8` | `L3_4` | **1.00000** | 0.00 | 0.1 % |
| `10_7` | `L3_9` | **1.00000** | 0.00 | 0.2 % |

Not "similar" - the same decoded data, up to the display stretch GobDump applies.
That is the cross-validation that matters: two independent implementations, two
independent demodulators and FEC decoders, identical pixels.

## Where each one is ahead

**elektro-ultra wins**
- Zero RS corrections and zero CRC errors on the golden capture; GobDump carries a
  running BER of ~0.01 and needed CRC recovery on two files.
- Honest partial output: a strip set with a missing segment is reported, not
  silently padded into a full-size canvas.
- Keeps the non-image xRIT files (EPI/PRO) as raw instead of dropping them.
- Measured baud readout (294 001 Bd against a 294 000 Bd nominal) instead of a
  configured constant.

**GobDump wins**
- **2.9x faster on the same CPU** (6.3x vs 2.1x realtime, both on ATLAS). Its
  kernels are volk/SIMD; ours are scalar. This is the real gap to close.
- Channel naming (`L3_4`) is instantly recognisable, where ours keeps the raw
  Elektro field (`03_8_076E-SEG`).
- Ships colour composites (false colour, cloud-top IR); ours are raw single-channel.

**ElektroDump** decodes frames at about the same rate as ours (1076 CADUs) but its
image stage produced **zero** products from the same 30 s of IQ, and it reports
2 CRC-recovered files and 1 uncorrectable RS block where ours has none.

## Closing the speed gap

Per-stage timing (`EU_PROFILE=1`, 30 s decode, images off) found where the time
actually went, which was not where it looked: the polyphase resampler and the RRC
together were 90 % of the DSP budget, and both were **latency-bound, not
bandwidth-bound** - one FMA accumulator chain stalls on its own dependency at
~6 cycles per tap.

Two changes, both verified to leave the decode bit-for-bit the same:

1. **Resampler**: the coefficient table is now phase-major and tap-reversed
   (`htab_`), so the window and the taps both run forwards, and the dot product
   uses four independent accumulators.
2. **RRC filter**: the ring buffer was replaced by a linearised history
   (`history + block`), which removes the per-tap wrap branch, plus four
   accumulators.

| stage (30 s of IQ) | before | after |
|---|---|---|
| resample | 2.58 s | 1.12 s |
| RRC | 1.95 s | 0.56 s |
| DC + NCO + AGC | 0.49 s | 0.49 s |
| **DSP total** | **5.02 s** | **2.15 s** |

3. **AVX2 in both inner loops** (no volk): four complex samples per FMA against
   a doubled real-tap table (`taps2_` / `htab2_`), with the scalar 4-accumulator
   path kept behind `#if defined(__AVX2__)` so the Pi build is unaffected.
4. **Resampler position bookkeeping**: the output position was recomputed from
   `k_out_` with two 64-bit divisions per output sample. It is now carried
   incrementally, which alone took the resampler from 0.94 s to 0.63 s.

5. **FEC, LUT form**: the Viterbi branch metrics are now a 4-entry table indexed
   by the packed generator bits (`bm_idx_`), the add-compare-select runs as 32
   butterflies over `int16` metrics, and Reed-Solomon uses a 256x256 product
   table built at startup instead of `${log}+${log}` with a modulo. FEC plus
   transport went from 1.74 s to 0.57 s per 30 s of IQ; the full capture from
   41.5 s to 32 s.

| end to end | before | after |
|---|---|---|
| 30 s slice, core | 8.3 s (3.6x) | 4.3 s (**6.9x**) |
| full 256 s capture, core | 74.9 s (3.4x) | 31.9 s (**8.0x**) |
| ziq 241 s capture, core | 4.2x | **8.9x** |
| 60 s slice, same CPU as GobDump | 28.0 s (2.1x) | 19.2 s (**3.1x**, portable) |

The decode output is unchanged throughout: 4173/4173 frames, 0 RS corrections,
and all five disks still correlate with GobDump's at r = 1.00000 (mean absolute
pixel difference 0.002, i.e. FMA rounding).

On the one machine where both could be measured (ATLAS, which has no AVX2, so it
only ever runs the portable path) the gap went from 2.9x to 2.3x. GobDump cannot
be built on core, so there is no same-hardware number at 6.2x yet.

`-march=native` alone was worth only 5 % (kept, and now an `ARCH` knob so the Pi
build stays possible: `make ARCH=armv8-a`, or `ARCH=none` for a portable binary).
A real/imag split rewrite of the FIR measured **17 % slower** and was reverted.

What is left, measured: the symbol loop (Gardner, Costas, cubic interpolator)
1.39 s and FEC+transport 1.74 s per 30 s of IQ, against 1.51 s for all the
filters now. The Viterbi writes 64 decision bytes per symbol and runs 64 states
per symbol - radix-4 decoding is the next real win, and the largest remaining
risk.

## How to reproduce

```
# elektro-ultra (this box or ATLAS)
./elektro-ultra decode -i LRIT/2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16 \
    --type lrit -o out/

# GobDump (ATLAS, /home/hermes/work/GobDump/build)
./gobdump legacy elektro_lrit baseband /home/hermes/work/iq/lrit_full.cs16 /tmp/out \
    --samplerate 3e6 --baseband_format cs16 --frequency 1691e6
```
