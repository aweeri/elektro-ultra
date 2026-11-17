# GROUND TRUTH - measured, not assumed

Everything in this file was measured on the actual box (Debian 13 container, i5-9500T,
6 cores, AVX2+FMA) or read out of the reference source trees that are already on disk.
Nothing here is recalled from memory or copied from docs.

Trees on disk used as reference:
- `/opt/data/cache/scratch/GobDump`  - MTalon63/GobDump (SatDump fork), commit "sqlite crash fix"
- `/opt/data/cache/scratch/ED`       - ElektroDump (Aria's own prior ground-station daemon,
                                       has validated RS/PN/deframer/xRIT code)

---

## 1. Sample corpus at cloud.a-where.com/share/IVY7anba

It is **File Browser 2.63.23**, so the share is a plain REST API, not a web app:

```
GET https://cloud.a-where.com/api/public/share/IVY7anba/<subdir>   -> JSON listing
GET https://cloud.a-where.com/api/public/dl/IVY7anba/<subdir>/<file> -> raw bytes
```

Range requests (`-r 0-500000000`) return `206 Partial Content`, so development can pull
head slices of the 7 GB files instead of the whole thing.

Share name: "Elektro L3 76E".

| Path | File | Bytes |
|---|---|---|
| HRIT | 2026-01-03_12-12-08_6000000SPS_1691000000Hz.cs16 | 7,708,409,856 |
| HRIT | 2026-01-03_15-12-07_3000000SPS_1691000000Hz.cs16 | 2,672,295,936 |
| HRIT | 2026-01-03_18-12-32_6000000SPS_1691000000Hz.cs16 | 4,656,857,088 |
| HRIT | 2026-01-04_09-12-09_6000000SPS_1691000000Hz.cs16 | 6,140,264,448 |
| HRIT | 2026-01-04_12-12-03_6000000SPS_1691000000Hz.cs16 | 5,588,975,616 |
| HRIT | 2026-01-05_15-12-01_10000000SPS_1691000000Hz.cs16 | 11,360,010,240 |
| HRIT | 2026-01-05_18-12-42_2560000SPS_1691000000Hz.cs8 | 955,809,792 |
| HRIT | 2026-01-06_00-12-57_2880000SPS_1691000000Hz.cs8 | 1,355,862,016 |
| LRIT | 2026-01-03_12-42-04_6000000SPS_1691000000Hz.cs16 | 10,879,696,896 |
| LRIT | 2026-01-03_15-43-02_3000000SPS_1691000000Hz.cs16 | 3,073,572,864 |
| LRIT | 2026-01-05_18-42-23_2880000SPS_1691000000Hz.cs8 | 2,382,769,152 |
| ziq | 2026-10-08_12-17-52_1500000SPS_1691000000Hz_old_l3_lrit.ziq | 473,390,992 |
| ziq | 2026-10-08_12-19-39_1500000SPS_1691000000Hz_old_l3_lrit.ziq | 310,906,574 |
| ziq | 2026-10-08_12-33-03_3000000SPS_1691000000Hz_old_l3_hrit.ziq | 738,942,662 |
| ziq | 2026-10-08_12-43-18_3000000SPS_1691000000Hz_old_l3_hrit.ziq | 697,956,615 |

Total ~59 GB. The box has 189 GB free, so a full mirror is possible but not necessary;
plan works from head slices plus 3-4 fully downloaded golden files.

## 2. Formats (confirmed by hexdump)

- `.cs16`: interleaved signed 16-bit little-endian I,Q. Verified: 400,000,001-byte tail
  divides by 4 with no remainder, and splitting as I/Q gives rms 0.034 with no clipping.
- `.cs8`: interleaved signed 8-bit I,Q. Same check, rms 0.011 to 0.025, values in +-6 LSB,
  i.e. these captures were recorded at very low gain (see section 4).
- `.ziq`: **22-byte header then a zstd frame**. Header bytes observed:

```
5a 49 51 5f   "ZIQ_"
01            version
08            bits per I/Q component (8 here)
60 e3 16 00   sample rate, LE u32  = 1,500,000
00 x 12       centre freq / start time / padding, all zero in these files
28 b5 2f fd   zstd magic at offset 22
```

Confirmed: `python zstandard` streaming decompress of `[22:]` produces 68,943,872 bytes of
8-bit interleaved IQ that contains a 294.0 kHz BPSK signal. So .ziq support = zstd + 22-byte
header, and the zstd path must be a *streaming* decompressor (a file may hold many frames).
Note `libzstd.so.1` (1.5.7) exists but there is **no zstd.h** and no `zstd` CLI - so the
loader must dlopen `libzstd.so.1` or vendor the decoder.

## 3. Parameters confirmed on real samples

**LRIT, 2026-01-03_15-43-02_3000000SPS (int16, 3 MSPS)**

- Cyclostationary peak of `|x|^2` at exactly **294.0 kHz** -> symbol rate 294,000 Bd.
- `x^2` tone at **+2.84 kHz** -> carrier offset ~**+1.42 kHz** from the file's nominal
  1691.000 MHz, with 46.6 dB peak-to-median. BPSK confirmed, not noise.
- No clipping (clip fraction 0.000), rms -29.5 dBFS.
- Spectrum peak +23.5 dB over the median bin.

**HRIT, 2026-01-04_09-12-09_6000000SPS (int16, 6 MSPS)**

- Cyclostationary line at **1150.0 kHz** (21 dB over median) -> symbol rate **1,150,000 Bd**.
- Spectrum occupies roughly +-2.5 MHz with 0.5 rolloff, consistent with RRC alpha 0.5:
  1.15e6 * 1.5 / 2 = 862 kHz half-band... measured edges are wider, so the capture has
  adjacent-channel energy; band-pass + RRC in the demodulator is required, not optional.
- A +11 dB spike sits at -1.0 MHz and +4 dB at DC: the signal is **not** centred at 0 Hz in
  this capture, and there is a DC spur. Both must be handled by acquisition, not by hope.

**HRIT, 2026-01-03_15-12-07_3000000SPS (int16, 3 MSPS) - SUSPECT**

- No 1.15 MHz cyclostationary line; the strongest was 857 kHz at 15 dB and the spectrum was
  flat to +-1.3 MHz with a hard shelf at -1.5 MHz. This looks like a capture where the signal
  is weak, off-centre or absent. Do not use it as a golden file until M0 triage says otherwise.

**HRIT, 2026-01-05_18-12-42_2560000SPS and 2026-01-05_18-42-23_2880000SPS (int8)**

- Both are near-empty: rms 1.4 and 1.1 LSB, spectrum flat within 1 dB, DC spur 40 to 49 dB
  above the median. These are low-gain / no-signal captures. Useful as **negative controls**
  for the deframer (a decoder must produce ~0 valid CADUs here), not as test material.

## 4. Reference chain, read out of GobDump

`resources/pipelines/Elektro_Arktika.json`, keys `elektro_lrit` and `elektro_hrit`:

| | LRIT | HRIT |
|---|---|---|
| frequency | 1691.0 MHz | 1691.0 MHz |
| samplerate | 1e6 | 6e6 |
| constellation | **bpsk** | **oqpsk** |
| symbolrate | 294000 | 1.15e6 |
| rrc_alpha | 0.5 | 0.5 |
| pll_bw | 0.0145 | 0.001 |
| max_sps | 3 | 3 |
| CADU | 8192 bits | 8192 bits |
| FEC | `ccsds_conv_concat_decoder` | same |
| viterbi_ber_threshold | 0.300 | 0.300 |
| viterbi_outsync_after | 20 | 20 |
| rs_i / rs_type | 4 / rs223 | 4 / rs223 |
| derandomize | true | true |

The FEC module takes **int8 soft symbols** into the Viterbi (`temp_soft_buffer`, per-symbol
`demod_soft_lut` for multi-bit constellations), then deframes with
`BPSK_CCSDS_Deframer(cadu_size=8192)` on the hard Viterbi output. So the reference already
does soft-decision Viterbi: our margin has to come from the front end, the sync strategy and
the outer code, not from "adding soft decisions".

`src-core/dsp/block.h` chain order for the fast path is RRC -> AGC -> Costas -> MM clock
recovery, and the hier block for `oqpsk` is still `logger->error("TODOREWORK!")`, i.e. the
OQPSK path only exists in the classic `module_psk_demod` path. That is a documented weak
spot in the reference, and a place we can clearly win.

## 5. Frame-rate arithmetic (corrects a myth in the docs)

CADU period = 8192 **channel** bits before rate-1/2 coding, so 16384 transmitted bits.

| Link | bits/symbol | symbol rate | channel bits/s | CADU/s |
|---|---|---|---|---|
| LRIT | 1 (BPSK) | 294,000 | 294,000 | **17.9** |
| HRIT | 2 (OQPSK) | 1,150,000 | 2,300,000 | **140.4** |

The old note "LRIT ~36 CADU/s" is off by exactly the rate-1/2 factor; it happens to look right
for HRIT because OQPSK doubles the bit rate. Any starvation check must use 17.9, not 36 -
otherwise a deframer running at 50% looks healthy.

## 6. Environment constraints that shape the plan

- `g++ 14.2.0` present. No cmake needed anyway (user asked for a Makefile).
- **No GLFW, no SDL2, no OpenGL or X11 headers, no fftw, no libpng, no libjpeg dev headers**,
  `pkg-config` has none of them, `ldconfig` is absent from PATH. Only `zlib.h` is available.
- Therefore: image decode via vendored `stb_image.h` (JPEG), image write via vendored
  `stb_image_write.h` (PNG), FFT written in-tree, zstd via dlopen of `libzstd.so.1`, and
  ImGui with a **software rasteriser first** (imgui_sw style) that can dump frames to PNG,
  with a GLFW/OpenGL backend enabled by the Makefile only when `pkg-config glfw3` succeeds.
- 6 cores, AVX2 + FMA -> runtime-dispatched SIMD, no baseline `-march=native` in shipped build.

## 7. Traps already paid for in this codebase (do not re-learn)

From `/opt/data/cache/scratch/ED/docs` and the xRIT audit:

1. RS(255,223) on the Elektro link arrives in **dual basis**. Without the conversion every
   codeword is uncorrectable while everything else looks healthy. Convert into polynomial
   basis *before* decoding.
2. RS depth-4 interleave is **strided**: `data[ii*4 + pos]`, never four contiguous blocks.
3. Derandomise **before** RS, and skip the first 4 bytes (ASM is not randomised). Reference
   taps 0x95, seed 0xFF, table starts `ff 48 0e c0 9a 0d 70 bc`.
4. A decimator that builds a proper FIR but dequeues through a 3-tap boxcar costs ~13 dB of
   stopband and folds noise into the signal band. Worth several dB, invisible in review.
5. Loop gains must be **normalised** (Gardner by `|s|^2+|mid|^2`, Costas by a slow power
   estimate). Un-normalised gains are ~30x wrong on a real front end and simply do not track.
6. Do not pair `I*Q` Costas detection with an unnormalised front end, and do not let the
   Costas inherit the PLL bandwidth (that cost 10-16% BER previously).
7. Never derive a carrier offset from the top bin of a wide FFT - use a narrowband `x^2`
   measurement, then confirm on the constellation.
8. A negative control matters: a 16-bit byte-sum "lock" once produced 2-10 fake frames on pure
   noise. Every sync claim gets tested against the two int8 noise captures above.
