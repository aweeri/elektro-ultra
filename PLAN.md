# Elektro-L Ultra - demodulator / decoder

**Status: M0-M5 executed. LRIT decodes end to end with zero RS corrections and
produces stitched full-disk images; HRIT is proven on synthetic CADUs but the
corpus has no usable HRIT capture. See RESULTS.md for measured numbers.**
**Author:** Hermes, for Aria
**Date:** 2026-11-16
**Companion file:** [`GROUND_TRUTH.md`](GROUND_TRUTH.md) - every measured fact this plan rests on

---

## 0. One paragraph

`elektro-ultra` is a single C++20 binary, plain `Makefile`, no CMake, that eats an IQ
recording (or a live device later) and pushes it through the best chain I can build for the
Elektro-L downlink: conditioning, optimal resampling and matched filtering, normalised timing
and carrier loops, soft demapping, soft-decision Viterbi, ASM correlation on soft symbols,
derandomisation, depth-4 deinterleave, RS(255,223) with dual-basis detection and erasure
retry, then xRIT reassembly to per-channel images. LRIT (BPSK 294 kBd) and HRIT (OQPSK
1.15 MBd) are the targets; GGAK is accepted, demodulated, and produces **no product output**
(blank, per instruction). It runs headless from the CLI, and the same run can open an ImGui
viewer with constellation, eye, spectrum, loop traces, FEC counters and live channel
thumbnails.

"Ultimate" is not a mood, it is a measurement: the plan is built so every claimed decibel is
attributable, tested against theory and against the reference decoder on the same recording.

## 1. Objectives and how success is measured

| # | Objective | Measure | Target |
|---|---|---|---|
| 1 | LRIT decode | CADU success rate on the golden 3 MSPS capture | >= 95% of the 17.9 CADU/s actually transmitted, and >= reference decoder on the same file |
| 2 | HRIT decode | CADU success rate on a golden 6 MSPS capture | >= 90%, and >= reference on the same file |
| 3 | Demod quality | implementation loss vs theory at BER 1e-5 | <= 0.5 dB (measured on a synthetic ladder) |
| 4 | FEC strength | input BER at which a CADU still decodes | >= 10% (Viterbi+RS envelope), reported as a curve |
| 5 | Products | channel images written | every channel present in the capture, correct dimensions, byte-identical to reference where both decode the same segments |
| 6 | Speed | offline throughput | >= 5x realtime at 6 MSPS on this 6-core box |
| 7 | Ruggedness | files that decode without manual parameters | 100% of the corpus after `--type auto`, zero files needing hand-tuned flags |
| 8 | Visibility | ImGui viewer | constellation, eye, spectrum, loop traces, FEC counters, 3-state lock colour |

Non-goals: live SDR capture (in scope only as a thin file/device abstraction, M9 option),
GGAK products, projection/georeferencing, web UI, multi-satellite support.

## 2. Where the decibels come from

The reference already uses soft-decision Viterbi (confirmed in
`module_ccsds_conv_concat_decoder.cpp`). So the plan cannot win by "adding soft decisions".
The realistic budget, with each item mapped to the stage that captures it:

| Source | Technique | Expected gain | Risk |
|---|---|---|---|
| Front-end conditioning | block DC removal + single-bin DC interpolation, blind IQ imbalance (Gram-Schmidt) | 0.5-2 dB EVM on cheap receivers | low |
| Anti-alias decimation | real polyphase FIR (Kaiser, 80 dB stop), not a boxcar | 1-3 dB where an integer decimator is used | low |
| Matched filter | RRC alpha 0.5, span 16 sym, correct gain, applied at 4 sps | 0.3-1 dB vs short/incorrect RRC | low |
| Timing | Gardner at 4 sps with cubic Farrow, error normalised by symbol energy | 0.5-1.5 dB at threshold, no periodic slips | medium |
| Carrier | separate acquisition (FFT + AFC) and tracking (narrow Costas) bandwidths, normalised error | 0.5-1.5 dB, removes the "clean cloud, bad BER" failure | medium |
| Soft ASM sync | correlate 32-bit ASM on **soft** bits before hard decisions, dual polarity, hunt window per frame | yield, not SNR: stops the deframer dropping into noise during fades | low |
| Rate-1/2 Viterbi | soft input, 64 states, traceback 96, metric renormalisation | baseline (reference has it) | - |
| **Erasure RS** | when a codeword exceeds t=32 errors, retry with low-reliability symbols marked as erasures (2e+f <= 32) | 0.5-1.5 dB of frame-level margin | medium |
| **Multi-hypothesis retry** | on CADU failure: retry +-1 bit slip, polarity flip, and 3 carrier-phase hypotheses over that window | recovers frames that are otherwise lost; large yield gain at low SNR | medium (cost) |
| **Decision-directed refinement** | after a CADU passes RS, use its 8192 known bits to re-estimate carrier phase and timing offset, feed back | 0.3-1 dB steady-state, kills slow drift | medium |
| **SOVA + soft RS** (stretch) | SOVA/BCJR LLRs, then algebraic soft-decision RS (Koetter-Vardy or reliability-ordered) | 0.7-1.0 dB | high, optional |
| Total, realistic | | **~4-7 dB better threshold and materially higher frame yield than a naive chain, ~2-4 dB better than the reference on its weak spots (OQPSK path, sync, outer code)** | |

Every row above becomes a measurable item in the validation plan (section 11), not a claim in
a README.

## 3. Ground truth in one table

Parameters are the reference's, verified against real captures (details and evidence in
`GROUND_TRUTH.md`).

| | LRIT | HRIT |
|---|---|---|
| RF | 1691.0 MHz | 1691.0 MHz |
| modulation | BPSK | OQPSK (QPSK fallback to be verified in M4) |
| symbol rate | 294,000 Bd (measured 294.0 kHz) | 1,150,000 Bd (measured 1150.0 kHz) |
| RRC | alpha 0.5 | alpha 0.5 |
| CADU | 8192 bits = 4-byte ASM + 4 x RS(255,223) | same |
| inner code | conv. rate 1/2, K=7, G1=0171 G2=0133 | same |
| outer code | RS(255,223), depth 4, **dual basis** | same |
| randomiser | CCSDS PN, taps 0x95 seed 0xFF, skip 4 bytes, before RS | same |
| ASM | 1ACFFC1D, polarity may be inverted | same |
| expected CADU/s | **17.9** (16384 channel bits per CADU) | **140.4** |
| sample rates in corpus | 3, 6 MSPS (cs16), 2.88 (cs8), 1.5 (ziq) | 3, 6, 10 (cs16), 2.56, 2.88 (cs8), 3 (ziq) |

## 4. Architecture

Layered, one library, three front ends (CLI, GUI, tests). No global state; the DSP chain is
an object graph that a config struct builds, so the same chain can be run offline, in a test
harness, or (later) from a live source.

```
                 +---------------------- elektro-ultra (bin)
                 |            |                 |
           cli_main       gui_main          selftest/bench
                 |            |                 |
                 +-------> core::Pipeline <------+
                              |
   +--------------------------+----------------------------------+
   |            |             |            |          |          |
 source      front_end      demod        fec        product    metrics
 (file/ziq/  (dc, iqbal,    (resample,   (viterbi,  (xrit      (snr, evm,
  device)     decim)         rrc, agc,    deframe,   segments,   ber, cadu
                             timing,      derand,    images)     rate, loop
                             costas,      deinter,               traces)
                             demap)       rs)
```

Threads (SPSC lock-free rings, fixed capacity, no allocation on the hot path):

```
reader -> [ring] -> front_end+demod (1 thread, SIMD) -> [ring of soft bits] -> fec (1 thread)
                                                                                   |
                                            metrics/UI snapshot (atomic struct) <--+
                                                                                   |
                                                                    product (1 thread, IO)
```

Rules: the demod thread never blocks on IO; the FEC thread never blocks on images; the UI
reads only an atomic snapshot struct (no locks, no allocation). Backpressure is explicit and
counted (dropped blocks are a metric, not a silent failure).

## 5. Repository layout

```
elektro-ultra/
  Makefile                 # plain make, no cmake, auto-detects optional deps
  README.md
  PLAN.md                  # this file (copied in)
  GROUND_TRUTH.md
  third_party/
    stb_image.h            # JPEG decode (public domain)
    stb_image_write.h      # PNG encode (public domain)
    imgui/                 # ImGui core only
    imgui_backends/        # glfw/opengl3 (optional) + sw render backend (always)
  src/
    main_cli.cpp
    main_gui.cpp
    core/
      config.h/.cpp        # arg + file + env resolution, presets
      pipeline.h/.cpp      # wiring, threading, stats snapshot
      sample_format.h/.cpp # cs16, cs8, cu8, cf32, ziq(zstd), auto-probe
      ring.h               # SPSC ring buffer
      stats.h              # atomic snapshot struct, JSON writer
      log.h/.cpp
    dsp/
      fft.h/.cpp           # owned radix-2/4, no fftw
      fir.h/.cpp           # windowed-sinc design, polyphase
      resampler.h/.cpp     # rational polyphase + Farrow cubic
      rrc.h/.cpp           # matched filter, 4 sps
      agc.h/.cpp           # slow linear power AGC
      timing.h/.cpp        # Gardner, normalised, 4 sps in, 1 sps out
      costas.h/.cpp        # BPSK + QPSK/OQPSK, normalised, slip detect
      acq.h/.cpp           # coarse freq (x^2/x^4 FFT + centroid), AFC
      demap.h/.cpp         # soft LLR -> int8, BPSK and I/Q parallel for OQPSK
      offtune.h/.cpp       # OQPSK half-symbol I/Q alignment search
      snr.h/.cpp           # EVM/SNR estimator + calibration
      metrics.h/.cpp       # eye, constellation histogram, loop traces (ring buffers)
    fec/
      viterbi.h/.cpp       # soft in, 64 states, optional SOVA output
      deframer.h/.cpp      # soft ASM correlator, hunt window, polarity
      randomiser.h/.cpp    # CCSDS PN, bit-exact vs reference table
      rs.h/.cpp            # RS(255,223), dual basis, erasures, BM + Forney
      deinterleave.h/.cpp  # depth-4 strided
      cadu.h/.cpp          # CADU -> VCDU -> packets
      fec_chain.h/.cpp     # the ordered chain + retry policy
    xrit/
      segment.h/.cpp       # primary/segment/image headers
      reassemble.h/.cpp    # by channel, coverage, gaps
      image.h/.cpp         # JPEG/raw decode, 8/10/16-bit, PNG write
      channels.h/.cpp      # Elektro channel table (derived in M0, verified on corpus)
    ggak/
      ggak.h/.cpp          # demod only, product output disabled
    gui/
      app.h/.cpp           # ImGui layout, panels
      panels_*.cpp         # constellation, spectrum, trace, fec, products, log
      sw_backend.h/.cpp    # software rasteriser -> PNG frames
  tests/
    unit_*.cpp
    synth_chain.cpp        # SNR ladder
    corpus_manifest.json   # written by M0 triage
  tools/
    triage.py              # corpus classification (python, dev only)
    compare_with_reference.py
```

## 6. Module design

### 6.1 Source and format layer
- `cs16`, `cs8`, `cu8` (DC at 127.5), `cf32`, `ziq` (22-byte header + zstd stream, dlopen
  `libzstd.so.1`, streaming frames), `auto` (probe by extension, then by size arithmetic and
  spectrum sanity).
- Sample rate and centre frequency from CLI, else from filename pattern
  `YYYY-MM-DD_HH-MM-SS_<rate>SPS_<freq>Hz`, else from the ziq header.
- All formats normalised to `complex<float>`; int8/int16 scaled by 1/128 and 1/32768 with a
  documented full-scale convention, so SNR numbers are portable.
- Big files: chunked reads (8 MB blocks), seekable, so a 11 GB file streams.

### 6.2 Conditioning
- Block DC removal (complex mean over 4096 samples) plus optional single-bin DC interpolation
  when a spur exceeds the local median by >20 dB. Switchable, because a BPSK RRC does carry
  energy at DC and a blind notch would eat signal.
- Blind IQ imbalance: Gram-Schmidt on the covariance of I and Q over ~1e6 samples, applied as
  a 2x2 real matrix. Reported as a metric (gain mismatch dB, phase error deg) in the nerd panel.
- Optional spectrum inversion (`--invert` -> conjugate) for LNB/LO below the carrier.

### 6.3 Resampling and matched filter
- Rational polyphase resampler from `fs` to exactly 4 sps (`4 * baud`), Kaiser-windowed sinc,
  ~64 taps per phase, 80 dB stopband, cutoff at `0.5 * min(1, out/in) * 1.05 * (1+alpha*0.0)`
  - the anti-alias is the resampler, deliberately *not* a boxcar decimator (see trap 4).
- RRC matched filter at 4 sps, alpha 0.5, span 16 symbols (65 taps), unity DC gain, no
  normalisation of output amplitude (the loops normalise instead).
- For HRIT at 6 MSPS this is a 4.6 MSPS complex stream through 65 taps: ~300 M MAC/s, fine
  with AVX2 and well under one core.

### 6.4 AGC
Slow linear power estimate (one-pole, tau ~ 2000 symbols), gain clamped, applied *before* the
timing loop but after the RRC so the matched filter sees real amplitudes. Loops use their own
normalisation, so AGC is a convenience for display and for the slicer thresholds, not a
crutch. `--agc off` exists because an unnecessary AGC is a real failure mode.

### 6.5 Timing recovery
- Gardner at 4 sps, error `(s[n] - s[n-1]) . conj(mid)`, **divided by** `|s[n]|^2 + |mid|^2`.
- Loop: proportional + integral on the fractional interval, cubic Farrow interpolator, output
  1 sps decisions; damping 0.707, `Bn*T` 0.005 tracking / 0.02 acquisition.
- Timing lock metric: variance of the timing error over a sliding window, exposed to the UI and
  used by the multi-hypothesis controller.
- OQPSK: the I and Q rails are offset by T/2. Alignment is found by maximising the
  cyclostationary metric of each rail independently (`|I|^2` and `|Q|^2` envelope line at
  Rs), then applying a half-symbol delay to one rail (`offtune.cpp`). This is the stage the
  reference does not have, so it gets its own acceptance test (M4).

### 6.6 Carrier acquisition and tracking
Three stages, each with a different job:
1. **Coarse** (once, or on loss of lock): `x^2` (BPSK) / `x^4` (QPSK) FFT with 4x zero
   padding and parabolic interpolation, searched only after band-limiting to the signal, plus
   a spectrum-centroid fallback for OQPSK where the M-th power line is weak. Accuracy target
   +-200 Hz. Bound the search: never accept a peak outside +-20 kHz without a flag.
2. **AFC / wide Costas**: `Bn*T` ~0.01, pull-in +-5 kHz, 4096 symbols, then hand over.
3. **Track Costas**: `Bn*T` 0.0015 for LRIT/HRIT (reference uses 0.001 for HRIT; we tune per
   link in M5), normalised error, separate from the timing loop's bandwidth by construction.
- Cycle-slip handling: detect a pi flip via ASM polarity on the next frame and correct by
  inverting the soft symbols, rather than letting the Viterbi eat a whole frame.
- `--carrier <Hz>` pins the offset when known; `--carrier auto` runs stage 1.

### 6.7 Demapping
- BPSK: one soft value per symbol, `LLR ~ 2*y/sigma^2`, scaled and clipped to int8 with a
  documented sign convention (positive = 1), plus a calibration constant so the Viterbi's
  thresholds mean what they say.
- OQPSK/QPSK: two soft values per symbol from I and Q rails after the half-symbol alignment.
- Optional 3-bit soft quantisation mode (8 levels) for comparison with 8-bit soft in the
  ladder - some decoders do better with companded metrics.

### 6.8 Metrics
- SNR: PSK SNR estimator (M2M4 style) **plus** spectrum-integration ground truth in the test
  harness. Both reported; the estimator is calibrated against the integrator so the number on
  screen is honest (an EVM estimate runs ~1.5 dB optimistic).
- EVM, symbol density histogram, eye diagram, timing/phase error traces, loop frequency
  estimate, block drop counters, throughput.

### 6.9 Viterbi
- Rate 1/2, K=7, G1=0171/G2=0133 (bit-reversed constants accepted, same code), 64 states.
- Soft input (int8), branch metrics precomputed, path metrics renormalised every 64 symbols,
  traceback depth 96, sliding window output.
- Node sync: re-encode decoded bits, measure BER, lock/unlock with hysteresis (threshold 0.30,
  outsync 20 symbols of clean run), exact per the reference semantics so our lock state is
  comparable on the same file.
- Optional SOVA: per-bit reliability from the surviving paths, for the stretch soft-RS stage.

### 6.10 Deframer (the place we can clearly beat the reference)
The reference deframes on **hard** Viterbi output. Ours:
- Correlate the 32-bit ASM (`1ACFFC1D`, and its inverse `E53003E2`) against **soft** bits,
  using a normalised correlation, so a marginal frame is scored rather than binarised.
- Hunt in a window around the nominal 8192-bit period (window +-40 bits) instead of stepping a
  fixed length, because the satellite clock and the sample clock drift; record the observed
  period as a metric (it is a free ppm estimate of the recording).
- Confidence gate: require two consecutive ASM hits at the same phase before declaring sync
  (kills the "locks on noise" class of bug, which has burned this project before), then track
  with hysteresis and per-frame confidence.
- Output: payload bytes + per-frame confidence + bit-phase, to the FEC stage; failing frames
  still go forward with a low confidence so the retry controller can act.

### 6.11 Randomiser
CCSDS PN, 255-byte table, taps 0x95, seed 0xFF, skip the first 4 bytes (ASM not randomised).
Bit-exactness against the reference table `ff 48 0e c0 9a 0d 70 bc` is a unit test.

### 6.12 Deinterleave and RS
- Strided depth-4 deinterleave: `data[ii*4 + pos]`. Never contiguous blocks.
- RS(255,223), field poly 0x187, first consecutive root 112, dual-basis transform applied on
  input (into polynomial basis), syndromes, Berlekamp-Massey, Chien, Forney.
- **Erasure support**: symbols whose Viterbi reliability is below a threshold are marked, and
  the codeword is retried under `2e + f <= 32`. Order: (a) plain error decode; (b) if it fails,
  erasure decode with the k least-reliable symbols; (c) if it still fails, retry the other
  basis and keep whichever passes the (optional) parity check.
- Counters: corrected symbols, uncorrectable, erasures used, basis flips. These are the numbers
  that let us prove the FEC stage is working rather than guessing.

### 6.13 CADU / VCDU / xRIT
- CADU -> VCDU header parse (version, SCID, VCID, counter), packet extraction with continuity
  counters, gaps reported per channel.
- xRIT files: primary header (file type 0 = image), image structure record (columns, rows,
  bits per pixel), image data function record, annotation, timestamp, segment identification.
  Elektro's segment identification record is the local 128-byte variant with a `compression`
  byte (offset 12), so the parser keeps both the standard and the Elektro variant.
- File type codes other than image are written as raw files into `raw/<channel>/` so nothing is
  silently dropped.

### 6.14 Images
- `compression` 0 = raw (8 or 10-bit packed), 1 = JPEG (vendored stb_image). Wavelet
  (DecompWT) is **not** used by Elektro as far as the corpus shows; if a channel turns out to
  be wavelet-coded in M3, that becomes an explicit, isolated addition rather than a speculative
  dependency.
- Per-channel PNG, 16-bit for IR/10-bit data, 8-bit for JPEG-sourced VIS, plus a
  `quicklook_rgb.png` composite where three VIS channels are present.
- Output tree:
  `out/<capture_stem>/LRIT|HRIT/ch<NN>_<name>_<timestamp>.png`, `.../raw/`, `stats.json`,
  `coverage.txt` (which segments were missing), `products.csv` (index).
- Missing segments are interpolated nowhere and reported explicitly - no fake pixels.

### 6.15 GGAK stub
`--type ggak` runs the demod and reports SNR/lock, then explicitly does nothing: no segments, no
images, no files, and a log line stating product output is disabled by design. Structure left
in place for a later CCSDS GGAK ingest.

### 6.16 Multi-hypothesis recovery controller (the "recovery brain")
Runs on the FEC side, cost-bounded, only for frames that fail:
1. **Bit slip +-1** on the frame's soft bits (protects against a single deframer phase error).
2. **Polarity flip** (cheap; covers a Costas slip we missed).
3. **Carrier hypotheses**: re-run the last 2 ms of IQ at 3 offsets (+-250 Hz) when a run of
   >5 frames fails and the phase-error trace shows a ramp.
4. **Clock hypothesis**: +-50 ppm resampling once, if the observed ASM period drift exceeds
   the hunt window.
Every retry is counted, and the controller disables itself for a file when the success rate of
retries drops below 1% (so a broken hypothesis cannot cost 3x throughput for nothing).

### 6.17 Decision-directed refinement
When a CADU passes RS, its 8192 information bits are known. Use them as a data-aided
reference: estimate residual carrier phase and timing offset over that frame with a
slide-window correlation, then correct the loop states and replay the buffered 2 frames.
This is the "iterative" part of iterative decoding; it is also the cleanest way to kill the
slow phase drift that shows up as periodic RS bursts.

## 7. CLI

`elektro-ultra <command> [options]`. Every option has a config-file equivalent, CLI wins.

```
elektro-ultra decode  -i FILE [options]      # the main path
elektro-ultra probe   -i FILE                # what is in this file? prints a suggested command
elektro-ultra synth   [options]              # write a synthetic IQ file (tests, teaching)
elektro-ultra selftest                       # FEC vectors + optional corpus replay
elektro-ultra bench   -i FILE --realtime N   # throughput measurement
```

### 7.1 The arguments that matter (and why)

| Flag | Meaning | Default |
|---|---|---|
| `-i, --input PATH` | IQ file, repeatable | required |
| `--type lrit\|hrit\|ggak\|auto` | transmission type; sets baud, mod, sps, loop presets | `auto` |
| `--format auto\|cs16\|cs8\|cu8\|cf32\|ziq` | sample format | `auto` |
| `--rate HZ` | sample rate of the recording (or from filename/header) | auto |
| `--center HZ` | RF centre the recording was taken at (bookkeeping, and DC/offset sanity) | auto |
| `--baud HZ` | symbol rate override | 294000 / 1.15e6 by type |
| `--mod auto\|bpsk\|oqpsk\|qpsk` | modulation override | by type (`auto` probes) |
| `--carrier HZ\|auto` | baseband carrier offset | `auto` |
| `--invert` | conjugate the spectrum (LO below carrier) | off |
| `--polarity auto\|normal\|inverted` | ASM polarity / bit sense | `auto` |
| `--sps N` | samples per symbol into the loops | 4 |
| `--rrc-alpha F`, `--rrc-span SYM` | matched filter | 0.5, 16 |
| `--pll-bw F`, `--pll-bw-acq F` | Costas bandwidths | 0.0015, 0.01 |
| `--clock-bw F` | Gardner bandwidth | 0.005 |
| `--agc on\|off\|auto`, `--agc-ref F` | AGC | auto |
| `--dc on\|off\|auto`, `--iq-balance on\|off\|auto` | conditioning | auto |
| `--cadu-bits N`, `--rs-depth N`, `--derand on\|off` | FEC geometry | 8192, 4, on |
| `--erasure on\|off`, `--mv on\|off`, `--dd-refine on\|off`, `--sova on\|off` | recovery features | on, on, on, off |
| `--threads N` | worker threads | 3 |
| `-o, --out DIR` | output root | `./out/<capture_stem>` |
| `--images on\|off`, `--png-depth auto\|8\|16` | product output | on, auto |
| `--dump-soft PATH`, `--dump-bits PATH`, `--dump-cadu DIR` | debug taps | off |
| `--stats PATH` | JSON stats | `<out>/stats.json` |
| `--json` | machine-readable progress on stdout | off |
| `--quiet`, `-v`, `--progress` | logging verbosity | normal, progress on tty |
| `--max-seconds S`, `--stop-after symbols\|cadus\|frames\|products` | bounded runs | off |
| `--gui`, `--gui-frames N`, `--gui-out PREFIX`, `--gui-scale F` | ImGui viewer, optionally headless to PNG | off |

Presets do the heavy lifting: `--type lrit` sets baud 294000, bpsk, 4 sps, alpha 0.5, the loop
bandwidths, CADU 8192, RS depth 4, dual-basis on. The user only has to say the type and
the rate; everything else is a knob for the nerd panel, not a prerequisite.

### 7.2 Behaviour contract
- Exit 0 = ran to completion. 2 = no lock ever. 3 = partial (some CADUs, some products).
  4 = bad input/format. 5 = internal assertion.
- One screen of output at the end: file, rate, snr dB, lock (not synced / syncing / synced),
  symbols, viterbi BER, CADU total/good, RS corrected, products written, throughput.
- `stats.json` carries the full counter set (schema in README) so results are diffable across
  builds and runs.

### 7.3 Examples

```
# LRIT from the golden capture, images out
elektro-ultra decode -i 2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16 --type lrit -o out/lrit_1543

# HRIT, unknown offset, everything automatic
elektro-ultra decode -i HRIT/2026-01-04_09-12-09_6000000SPS_1691000000Hz.cs16 --type hrit

# just tell me what this file is
elektro-ultra probe -i 2026-10-08_12-19-39_1500000SPS_1691000000Hz_old_l3_lrit.ziq

# watch it work
elektro-ultra decode -i capture.cs16 --type hrit --gui

# headless viewer frames (this container has no GL)
elektro-ultra decode -i capture.cs16 --type lrit --gui --gui-frames 120 --gui-out /tmp/eu_%04d.png

# regression, no images, timed
elektro-ultra selftest --corpus tests/corpus_manifest.json --max-seconds 20
```

## 8. ImGui viewer

One window, dense, instrument-panel feel (the station house style: white paper, IBM Plex,
petrol accent, 3-state lock colour). Panels:

1. **Header strip**: file, type, rate, symbol rate, SNR, EVM, throughput, and the lock state in
   SatDump's three colours (not synced / syncing / synced) with both words and dot.
2. **Constellation**: density plot (log heatmap, not a scatter of 50k points), decision
   boundaries, EVM circles, and a freeze/zoom control.
3. **Eye diagram**: I and Q rails, 2-symbol span, persisted fade over time.
4. **Spectrum + waterfall**: the same instrument the station uses (power-averaged rows,
   autoscaled from the median bin, dB window printed). Shared column axis.
5. **Loop traces**: timing error, phase error, AGC gain, carrier frequency estimate - 4 small
   sparklines with the loop bandwidths annotated.
6. **FEC panel**: Viterbi metric, node-sync BER, ASM confidence, CADU rate vs expected (17.9 /
   140.4), RS corrected / uncorrectable / erasures / basis flips, retry success rate.
7. **Frame timeline**: last 200 CADUs as coloured ticks (ok / corrected / failed / retried), so
   burst error structure is visible at a glance.
8. **Products**: per-channel thumbnail, dimensions, bit depth, segment coverage %, missing count.
9. **Log**: last N lines, filterable.

Controls: pause/step, reset loops, nudge carrier +-50 Hz, toggle conditioning stages live,
change traceback depth, dump current frame's soft bits. Because this is a decoder for
diagnosis as much as for pictures.

Backends: software rasteriser (always available, writes PNG frames, works in this container)
plus GLFW/OpenGL3 when `pkg-config glfw3` succeeds. The GUI reads only the atomic snapshot, so
it can never slow or stall the DSP.

## 9. Config file

`elektro-ultra.toml`-ish (own minimal parser, no dependency): sections `[input]`, `[demod]`,
`[fec]`, `[product]`, `[gui]`, `[log]`. Precedence: CLI > env (`EU_*`) > `./elektro-ultra.conf`
> `~/.config/elektro-ultra/conf` > built-in defaults. Config is echoed into `stats.json` so a
result is always reproducible.

## 10. Performance and SIMD

- Baseline build: `-O3 -fno-math-errno`, no `-march=native` in shipped binaries.
- Hot kernels (RRC, resampler, Viterbi ACS, soft ASM correlation, RS syndromes) have
  `__attribute__((target("avx2,fma")))` variants selected by runtime CPUID, so the binary stays
  portable.
- Targets on this box: >= 5x realtime at 6 MSPS, >= 3x at 10 MSPS, memory flat (streaming, no
  full-file load), < 200 MB RSS for a 11 GB input.
- Viterbi is the likely hotspot at 2.3 Mbit/s; if it becomes the limit, the ACS gets the
  classic 4-state-in-parallel SIMD layout with a butterfly table.

## 11. Validation plan

Everything here is a gate, not a suggestion. Nothing is called "working" without a number.

### 11.1 Synthetic ladder (no recordings involved)
- `synth` writes BPSK 294 kBd and OQPSK 1.15 MBd at 2/3/6/10 MSPS, RRC 0.5, with AWGN at
  Es/N0 from -2 to +12 dB in 0.5 dB steps, plus impairments: carrier offset +-50 kHz, clock
  offset +-100 ppm, IQ gain 1 dB / phase 3 deg, phase noise.
- Measure: BER vs theory, CADU success vs Es/N0, implementation loss. Gate: <= 0.5 dB.
- **Ground truth by spectrum integration**, not by the tool's own readout.
- Regression: this whole ladder runs in `make test` in under a minute at 3 MSPS.

### 11.2 Corpus
- M0 produces `tests/corpus_manifest.json`: per file, measured symbol rate, carrier offset,
  in-band SNR, presence verdict, and role (golden / negative control / skip).
- Golden set: one LRIT 3 MSPS cs16, one HRIT 6 MSPS cs16, one ziq LRIT (zstd path), one cs8
  that is a real signal if triage finds one.
- Negative controls: the two int8 near-empty captures must produce **0** RS-correctable CADUs
  and 0 images. Any non-zero result is a bug in sync, not a discovery.
- Per-file results table (CADU/s vs 17.9 / 140.4, RS stats, products) committed as
  `tests/corpus_results.md`.

### 11.3 A/B against the reference
- Same recording, same start, GobDump (already on disk) vs `elektro-ultra`.
- Compare: CADU yield, RS corrected count, images decoded per channel, and pixel-diff of
  images produced from the same segments (must be identical; any difference is a bug in one of
  them, most likely ours).
- Report the delta honestly, including where the reference wins.

### 11.4 FEC stress
- Take a clean decoded CADU stream, inject bit errors at 1e-4 .. 1e-1, measure decode envelope
  with and without erasures. Plot it. This is the "FEC recovery" claim, quantified.

### 11.5 Unit tests
PN table bytes, RS syndrome-zero on a known codeword, dual-basis round trip, Viterbi round
trip, strided deinterleave against a hand-built vector, ASM on noise (must not lock),
resampler impulse response, ziq header parse.

## 12. Milestones

Each milestone has a gate. If a gate fails, the next milestone does not start; we fix or
re-plan. Effort is rough active-work hours on this box.

| # | Milestone | Deliverable | Gate | Est. |
|---|---|---|---|---|
| **M0** | Skeleton + triage | Makefile, format layer (incl. ziq/zstd), `probe`, `synth`, corpus manifest | 15 corpus files classified with symbol rate and SNR; ziq decodes | 2-3 h |
| **M1** | DSP core | resampler, RRC, AGC, Gardner, Costas, acq, demap, metrics | synthetic BPSK/OQPSK ladder within 0.5 dB of theory | 5-7 h |
| **M2** | FEC core | Viterbi, deframer (soft ASM), randomiser, deinterleave, RS dual-basis + erasures | synthetic end-to-end CADUs decode at the designed BER envelope; noise produces zero frames | 5-7 h |
| **M3** | LRIT end to end | full chain on the golden LRIT capture, images out | >= 95% CADU yield, images match reference | 4-6 h |
| **M4** | HRIT end to end | OQPSK rail alignment, HRIT presets, images out | >= 90% CADU yield on a 6 MSPS capture | 5-7 h |
| **M5** | Ruggedness | auto type/mod/offset, polarity, cycle-slip repair, multi-hypothesis, DD refinement | all corpus files decode with zero manual flags; retry controller measurable | 6-10 h |
| **M6** | Products polish | segment coverage, channel table, 16-bit PNG, quicklook, products.csv | coverage report matches segments seen; no fake pixels | 3-5 h |
| **M7** | ImGui viewer | panels + software backend + PNG dump, optional GLFW | viewer renders in this container; three-colour lock verified by driver script | 4-6 h |
| **M8** | Benchmarks + docs | results table, throughput, README, A/B report | GO/NO-GO review vs reference | 3-5 h |
| **M9** | Stretch | SOVA + soft-decision RS, live SDR source | only if M1-M8 are green | - |

Ordering note: M1 and M2 are separable and can run in parallel if I delegate; M3 depends on
both. M7 can start any time after M3 because the snapshot struct exists from M0.

## 13. Risks and unknowns

| Risk | Impact | Mitigation |
|---|---|---|
| HRIT is OQPSK but our rail alignment is wrong | no HRIT decode | M4 starts with a dedicated alignment experiment on a 6 MSPS capture and a synthetic OQPSK reference; QPSK fallback flag |
| The HRIT 3 MSPS capture is not a real signal | wasted hours | excluded from golden set by M0 triage; 6 MSPS files carry the milestone |
| Elektro xRIT uses a compression we do not have (wavelet) | no images | M3 checks the `compression` byte across every segment in the corpus before writing the image stage; stb JPEG handles the common case |
| Channel table invented rather than derived | wrong filenames, wrong composites | table is built from the corpus's own headers in M0/M6 and cross-checked against GobDump's identify table |
| No GL/X11 in this container | no viewer to verify | software rasteriser first, GLFW backend conditional; PNG frame dump is the verification artefact |
| Corpus is 59 GB | disk pressure | head slices for dev, 4 golden files fully mirrored (~20 GB), manifest records byte ranges used |
| We copy reference code by accident | licence + correctness | GobDump is GPLv3: reference only, no code lifted; our own GPLv3 release, attribution in README |
| Viterbi or RS too slow at 10 MSPS | throughput gate | SIMD ACS in M1/M2, measured each milestone; throughput is a gate, not a wish |

## 14. Licensing and attribution

- Our code: GPLv3 (matching the ecosystem it will be compared to), own implementation.
- Vendored: `stb_image.h`, `stb_image_write.h` (public domain), ImGui (MIT), imgui_sw-style
  rasteriser (MIT). `libzstd` used via dlopen at runtime, not vendored.
- GobDump / SatDump: reference for parameters and behaviour only, credited by name in the
  README with the exact files consulted. No source copied.
- Aria's own ElektroDump code (in `scratch/ED`) is her own work and may be reused for the RS /
  PN / deframer pieces where it is already validated - I will say explicitly which files are
  reused rather than re-derived.

## 15. Open questions to settle at GO

1. **Name**: `elektro-ultra` (binary `elektro-ultra`) or something else? Default is that.
2. **GGAK**: confirmed "demodulate and report, produce nothing, keep it blank" - no CSV, no
   frames on disk until told otherwise.
3. **Corpus policy**: OK to download the 4 golden files (~20 GB) into
   `/opt/data/cache/scratch/elektro-ultra/data/`, and to build GobDump here for the A/B?
   (GobDump is already on disk; building it is ~20-40 min of CPU.)
4. **Image output preference**: PNG per channel (default) or also a 3-channel RGB quicklook and
   a combined false-colour composite? Default: per-channel PNG + RGB quicklook when three VIS
   channels exist.
5. **Language of UI strings**: English (default) or Polish?

## 16. What I will do first when told GO

M0 only, in this order: repo skeleton and Makefile, format layer with ziq/zstd, `probe` and
`synth`, then the corpus triage that writes the manifest. That produces a table of what is
actually in these recordings - which is the fact base for every later decision - and it is
reviewable before any demodulator code exists.
