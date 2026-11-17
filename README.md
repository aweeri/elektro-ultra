# elektro-ultra

An Elektro-L LRIT / HRIT demodulator and decoder: one C++20 binary, a plain
Makefile, no CMake, no external DSP library. It eats an IQ recording, recovers
the symbols, decodes the CCSDS concatenated code, and writes the xRIT products as
PNG. The same binary can open an ImGui viewer for the constellation, the spectrum
and the loop/FEC counters.

```
make -j$(nproc)
./elektro-ultra probe  -i capture.cs16
./elektro-ultra decode -i capture.cs16 --type lrit -o out/
./elektro-ultra decode -i hrit.wav16 --type hrit --mod qpsk -o out/
./elektro-ultra decode -i capture.cs16 --type lrit --gui --gui-frames 60 --gui-out frame%03d.png
```

## What it does

```
IQ file -> DC block -> NCO -> rational resample (4 sps) -> RRC matched filter
        -> [OQPSK rail alignment] -> AGC -> Gardner timing -> Costas
        -> soft demap (int8) -> Viterbi r=1/2 K=7 -> soft-ASM deframe
        -> derandomise -> depth-4 deinterleave -> RS(255,223) dual basis
        -> VCDU -> M_PDU -> CCSDS packets -> xRIT files
        -> JPEG / CCSDS-122 wavelet / raw -> PNG -> stitched full disk
```

Every stage is a small object with no global state, so the chain runs headless,
from the viewer, or from a test harness.

## Measured behaviour

On the LRIT captures in the corpus (an Elektro-L 3 MSPS recording and its
1.5 MSPS zstd-compressed sibling), whole-file runs give:

| | 3 MSPS cs16 (256 s) | 1.5 MSPS ziq (242 s) |
|---|---|---|
| CADUs decoded | **4175 / 4175** | **4147 / 4147** |
| CADU rate | 16.3 /s | 17.1 /s |
| RS corrections needed | 0 | 0 |
| RS failures | 0 | 0 |
| in-band SNR | 10.2 dB | 10.1 dB |
| EVM | 30.9 % (theory 30.5 % at this Es/N0) | 31.3 % |
| Viterbi BER | 0.0000 | 0.0000 |
| throughput | 6.9x realtime | 7.1x realtime |
| products | 30 PNGs, 5 stitched disks | 30 PNGs, 5 stitched disks |

4175 is the same frame count GobDump reports on that capture, and the stitched
disks correlate with theirs at r = 1.00000 (mean |diff| 0.001 of 255).

"0 RS corrections" is the strong claim: the demodulator plus Viterbi produce
frames that are already error-free, so the FEC has nothing left to do. The EVM
sits within half a dB of the matched-filter bound, which is the numeric form of
"no implementation loss left on the table".

HRIT is verified against a reference CADU file: **4799 / 4799 frames, every
payload byte-identical**, 19 wavelet products decoded to PNG, 3 full 2784x2784
disks stitched. Sensitivity, on the same stream with noise added in software:
every frame decodes down to **4.0 dB Es/N0**, one frame lost at 3.9, nothing at
3.8. See `RESULTS.md` for the sweep.

## Commands

| command | what it does |
|---|---|
| `probe` | measures level, band, symbol rate, modulation marks, in-band SNR and prints a suggested decode line |
| `decode` | the chain above; writes products, optionally the viewer |
| `synth` | writes a synthetic BPSK/QPSK/OQPSK recording for the SNR ladder (`--full-chain` builds real CADUs) |
| `noisify` | adds calibrated noise to a recording for a target Es/N0 |
| `selftest` | FEC and DSP vectors (see `tests/`) |
| `bench` | throughput measurement |

Options that matter: `--type lrit|hrit|ggak|auto`, `--format auto|cs16|cs8|cu8|cf32|ziq|wav16`,
`--rate`, `--center`, `--baud`, `--mod`, `--carrier auto|<Hz>`, `--invert`,
`--sps`, `--pll-bw`, `--pll-bw-acq`, `--clock-bw`, `--rrc-alpha`, `-o DIR`,
`--gui`, `--dump-soft`, `--dump-cadu`, `--max-seconds`.

Presets carry the link parameters: `--type lrit` sets 294 kBd, BPSK, alpha 0.5 and
the loop bandwidths; `--type hrit` sets 1.15 MBd, and `--type auto` measures the
symbol rate and modulation marks with `probe` and picks the preset. `--type hrit`
defaults to OQPSK - on a QPSK signal that is EVM ~40 % and zero frames, so retry
with `--mod qpsk` before suspecting anything else.

GGAK is accepted and demodulated, and deliberately produces **no product output**.

## Layout

```
src/core/    logging, sample formats (cs16/cs8/cu8/cf32/ziq+zstd/wav), FFT, analysis
src/dsp/     filters, resampler, demod chain (timing, carrier, demap), modulator
src/fec/     Viterbi, soft-ASM deframer, CCSDS randomiser, Reed-Solomon, chain
src/xrit/    VCDU/M_PDU/packet demux, xRIT headers, products, wavelet, PNG writer
src/cli/     option parsing and the command implementations
src/gui/     ImGui viewer + software rasteriser
tests/       per-stage truth tests (resampler, FracDelay, detectors, products)
tools/       fecprobe (FEC-only ladder), fec-sweep.sh, chanber (channel BER probe)
third_party/ ImGui (MIT), stb_image (public domain, patched),
             decompwt (EUMETSAT PublicDecompWT, Apache-2.0)
```

## Design notes worth knowing

- **Timing before carrier.** Gardner's TED is rotation invariant, so the clock loop
  runs first and the Costas runs at symbol rate on clean samples. Running the
  carrier loop at full rate costs 10-16 % BER here.
- **Timing loop bandwidth is in Hz, not normalised.** 2500 Hz at 1.15 MBd injected
  enough jitter to cost 0.6 dB; 1000 Hz is the default and moves the HRIT
  threshold from 4.5 dB down to 3.9 dB Es/N0. Going to 500 Hz loses tracking over
  a 34 s capture.
- **Two carrier bandwidths.** A wide loop acquires (a few kHz of pull-in), then the
  narrow loop tracks; the integrator carries the frequency estimate across the
  switch. `--pll-bw 0` means no loop at all, which is a real value, not "default".
- **The timing loop has a false lock at T/2.** The S-curve is zero there with the
  opposite slope, so acquisition scans fractional phases for maximum `|y|^2`
  instead of starting from an integer guess.
- **Soft ASM correlation.** The deframer scores the sync word against the soft
  bits instead of binarising first and re-guessing per frame. A threshold of 0.72
  keeps it from locking on noise, and two ASMs at the frame spacing must agree
  before anything is emitted.
- **Polarity is normalised by the deframer, never by the FEC.** `emit()` flips
  every bit when a frame only matches the inverted ASM. An FEC-side reaction to
  that used to re-invert frames that were already fixed; removing it recovered 14
  frames per HRIT capture and made BPSK streams whose carrier settles on the
  opposite phase decodable at all.
- **Resampler prototype is normalised per phase.** Normalising the whole prototype
  to unity DC gain divides the output by L and silently costs 40 dB.
- **Elektro's JPEG encoder writes a non-zero Ah/Al byte** in a sequential SOS,
  which stock libjpeg rejects as "bad SOS". The vendored stb_image ignores Ah/Al
  for sequential frames, and EOI terminates an xRIT JPEG payload.
- **HRIT wavelet is CCSDS 122**, decoded with EUMETSAT's PublicDecompWT. The
  decompressed buffer is packed at the declared bit depth (10 bits here, 1.25
  bytes per pixel), not 16-bit aligned.

## Limits

- No live SDR input yet (file and zstd stream only).
- GGAK products are not produced (demodulation only, by choice).
- Georeferencing/projection is not implemented; stitched disks are raw geometry.
- The viewer is headless in this container (software rasteriser -> PNG). Wiring a
  GLFW backend is a small addition, not an architectural one.
- Speed: resampler and RRC dominate. AVX2 fast paths exist, no volk by design.

## Licence

GPLv3. Vendored: ImGui (MIT), stb_image (public domain), EUMETSAT PublicDecompWT
(Apache-2.0, see `third_party/decompwt/LICENSE`). SatDump/GobDump was consulted as
a reference for parameters and conventions; no code copied from it.
