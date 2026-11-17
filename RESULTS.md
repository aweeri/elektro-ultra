# Measured results

Everything below comes from real runs of `./elektro-ultra` on the corpus in
`/opt/data/cache/scratch/elektro-ultra/data/` (~25.8 GB, 6 files), on this box
(i5-9500T, 6 cores, -O3, single threaded decode).

## Corpus triage (`probe`)

| file | rate | symbol rate measured | verdict |
|---|---|---|---|
| LRIT/2026-01-03_15-43-02 cs16 | 3 MSPS | 294 001 Bd, x^2 46.9 dB | **golden** - LRIT BPSK |
| LRIT/2026-01-03_12-42-04 cs16 | 6 MSPS | 2 010 545 Bd, in-band SNR -3.9 dB | not LRIT (different signal) |
| HRIT/2026-01-04_09-12-09 cs16 | 6 MSPS | 1 150 004 Bd, band 2.0 MHz, SNR 3.6 dB | HRIT but far too weak |
| HRIT/2026-01-03_18-12-32 cs16 | 6 MSPS | band 11 kHz wide, 2 010 521 Bd | not HRIT |
| ziq/...old_l3_lrit.ziq | 1.5 MSPS | 294 001 Bd, x^2 48.9 dB | **golden** - LRIT, zstd path |
| ziq/...old_l3_hrit.ziq | 3 MSPS | 1 003 121 Bd, band 27 kHz | not HRIT |

The symbol rate estimator was checked against synthetic signals with known
parameters: 280 000 Bd -> 280 000 Bd, 294 000 Bd -> 293 999 Bd.

## Full-file decode, LRIT

| metric | 3 MSPS cs16 | 1.5 MSPS ziq |
|---|---|---|
| IQ duration | 256.1 s | 241.9 s |
| CADUs decoded | 4173 | 4143 |
| CADU rate (file average) | 16.3 /s | 17.1 /s |
| RS corrections | 0 | 0 |
| RS failures | 0 | 0 |
| Viterbi BER (cumulative) | 0.0000 | 0.0000 |
| in-band SNR | 10.2 dB | 10.1 dB |
| EVM | 30.9 % | 31.4 % |
| carrier | +1418.7 Hz | +1417.2 Hz |
| clock (measured) | +2.4 ppm | +4.7 ppm |
| throughput | 15.0x realtime | 21.8x realtime |

`EU_PROFILE=1` prints per-stage DSP time (dc, nco, resample, rrc, rail, agc);
that is how the 1.55x speedup below was found instead of guessed.

EVM at Es/N0 = 10.2 dB has a matched-filter bound near 30.5 %, so the demodulator
sits within ~0.4 pp of theory. Zero RS corrections over 4173 frames means the
inner (Viterbi) decode is frame-exact: the outer code has nothing to do.

The gap between the file-average CADU rate (16.3/s) and the nominal 17.9/s is
**roughly 9 % of the file with no signal**, not lost frames: scattered losses
would show up as RS failures and broken images, and there are none. The ziq
capture, recorded with a different recorder, sits at 17.1/s, which supports the
same reading.

## Throughput

Same captures, same bit-exact output (4175/4175 CADUs, 0 RS corrections, stitched
disks still correlate r = 1.000000 with GobDump), 256.1 s of IQ:

| stage | before | after |
|---|---|---|
| dc | 1.79 s | 1.79 s |
| nco | 2.07 s | 1.45 s |
| resample | 5.55 s | 3.36 s |
| rrc | 3.22 s | 2.13 s |
| agc | 0.40 s | 0.42 s |
| symbol loop | 11.5 s | 5.1 s |
| fec + transport | 5.9 s | overlapped |
| **total** | **33.3 s (8.3x)** | **17.1 s (15.0x)** |

What each change bought:

- **The symbol loop was spending more on `% 64` than on the filters.** The timing
  loop pushed every resampled sample into a 64-entry ring and indexed it four
  times per symbol with a modulo. It now indexes a contiguous block buffer
  directly, jumping sample to sample instead of checking every one.
- **Closed-form resampler positions.** Output `t` sits at
  `base0 + floor((ph0 + t*M)/L)` with phase `(ph0 + t*M) % L`, so the whole
  output range is addressable up front and splits across threads without any
  cross-chunk state.
- **Block-parallel FIR and NCO.** Each RRC output reads a disjoint window of the
  history buffer, so the range splits with no locking. The NCO phase is analytic
  in the sample index, so a chunk starts exactly with one `cos`/`sin`.
- **FEC off the critical path.** Frames are independent once deframed, so
  derandomise + RS decode runs across the pool, and the whole FEC stage runs on
  its own thread behind the demod.
- **Thread count follows the cgroup quota.** `nproc` reports 6 here but
  `cpu.max` grants 2 CPUs; six worker threads against a 2-CPU CFS quota throttle
  the entire process and ran *slower* than two.

Two concurrency bugs found while getting this working, both worth writing down:
a worker could pick up a work item after the generation changed and call the
previous generation's `std::function`, and two threads calling `parallel_for`
concurrently clobbered the pool's shared generation state (this deadlocked the
ziq path, which runs FEC on one thread and the demod on another).

## Products

| metric | 3 MSPS cs16 | 1.5 MSPS ziq |
|---|---|---|
| CCSDS packets | 347 | 347 |
| packets with bad CRC | 0 | 0 |
| xRIT files parsed | 32 | 32 |
| files complete | 30 | 30 |
| image segments | 30 (2784x464, 8 bit, JPEG) | 30 |
| per-segment PNG | 30 | 30 |
| **stitched full disks** | **5 (2784x2784)** | **5** |
| unparsed (EPI/PRO) | 2 raw | 2 raw |

Five channels (three on `00_6/00_7/00_9`, plus `03_8` and `10_7`), six strips each,
stitched vertically into whole disks. The stitched images are auto-stretched to
the 0.5/99.5 percentile for viewing; the per-strip PNGs are left untouched so
any measurement stays honest. The output was visually verified: a coherent Earth
disk, no seams, no duplicated or misordered strips.

## HRIT

There is exactly one HRIT-looking capture in the corpus and it is below threshold:

| metric | HRIT/2026-01-04_09-12-09 |
|---|---|
| symbol rate | 1 150 004 Bd (nominal) |
| occupied band | 2.03 MHz |
| in-band SNR | 3.6 dB (Es/N0 ~5 dB) |
| EVM | 55.7 % |
| CADUs decoded | **0 / 255** |
| hypothesis hunt | all 4 carrier phases tried, none syncs |

The chain itself is proven on synthetic HRIT (real CADUs: ASM + four interleaved
RS(255,223) blocks + randomiser + rate-1/2 conv, OQPSK 1.15 MBd at 6 MSPS):

| Es/N0 | CADUs good | RS failures |
|---|---|---|
| 20 dB | 549 / 549 | 0 |
| 9 dB | 320 / 334 | 14 |
| 6 dB | 37 / 48 | 11 |

At 5 dB Es/N0 a rate-1/2 K=7 code still gives a usable bit stream, so the corpus
capture should decode; the open suspicion is that its transmitter filtering is not
RRC(0.5) (the band is flat-topped and notched in the middle) and the matched
filter is therefore mismatched. A clean HRIT capture is still needed.

## FEC performance against noise (ground truth)

The `sdr_test_setup` share gives a clean HRIT stream with known contents, so noise
can be added in software and the result checked against the reference CADUs.
`noisify` adds complex noise for a target Es/N0 (sigma^2 = P*(fs/Rs)/snr, checked
against the measured noise power to within 0.3 dB).

34 s of signal, 4799 frames, QPSK 1.15 MBd at 4.6 MSPS, AGC and DC off, carrier
loop at the default (1.5e-3, 3e-3 while acquiring), timing loop 1000 Hz:

| added Es/N0 | MER | M2M4 | frames good | RS corrected |
|---|---|---|---|---|
| 8 dB | 8.1 dB | 7.9 dB | **4799/4799** | 13 |
| 6 dB | 6.7 dB | 5.8 dB | **4799/4799** | 216 |
| 5 dB | 6.1 dB | 4.8 dB | **4799/4799** | 823 |
| 4.5 dB | 5.8 dB | 4.3 dB | **4799/4799** | 1804 |
| 4.2 dB | 5.6 dB | 4.0 dB | **4799/4799** | 3197 |
| 4.0 dB | 5.5 dB | 3.8 dB | **4799/4799** | 5217 |
| 3.9 dB | 5.3 dB | 3.3 dB | 4798/4799 | 6795 |
| 3.8 dB | 5.1 dB | 2.4 dB | 0/2194 | all failed |

The same sweep with the previous timing-loop bandwidth (2500 Hz) fell over at
4.5 dB: at 4.2 dB it decoded a fraction of the stream. Tightening the loop to 1000 Hz moved the threshold down **0.6 dB** and is
now the default. The cliff is now between 3.8 and 3.9 dB: 3.9 dB loses one frame
of 4799, 3.8 dB decodes nothing. Tightening further (500 Hz) loses tracking over a
34 s capture and decodes nothing even at 4.0 dB, so 1000 Hz is the sweet spot.

## Where the low-SNR limit actually is

Two measurements settle the "we should work at 1.5 dB" question, both on the
ground-truth HRIT stream.

**1. FEC alone, ideal soft bits, no demodulator** (`fecprobe`: the exact
transmitted channel bits plus calibrated noise, pushed straight into the FEC
chain):

| Es/N0 | frames good | RS failed |
|---|---|---|
| 6.0 dB | 1200/1200 | 0 |
| 5.0 dB | 1196/1199 | 3 |
| 4.5 dB | 1030/1195 | 165 |
| 4.0 dB | 15/1181 | 1166 |
| 3.5 dB | 0/427 | 427 |

**2. End to end** (same stream, through the demodulator): perfect at 4.5 dB,
dead at 4.0 dB.

The two curves fall over in the same place, within a fraction of a dB (the
ideal-soft ladder is slightly the harsher of the two, because its noise model is
an approximation of the real channel). Read that as: the demodulator is not where
the margin is going - the chain as a whole needs about 4.5-5 dB Es/N0 and that is
where rate-1/2 K=7 plus RS(255,223) stops working.

Caveat worth keeping honest: a per-window channel-BER measurement against the
transmitted bits was attempted (`tools/chanber.cpp`) and never produced a number
that agrees with the frame results, so it is not quoted. The frame-level ground
truth below is the check that stands.

## Why "1.5 dB" in a SatDump screenshot is not 1.5 dB Es/N0

SatDump's SNR box is an M2M4 estimate on the symbol stream (exponential average,
alpha 0.001). GobDump's own BER box is also scaled: `get_ber()` multiplies the
re-encode mismatch by 2.5, so a displayed BER of 0.14 is a raw 0.056.

A working GobDump LRIT decode shows, on the same symbols:

```
SNR (dB) avg 1.339   MER 4.65 dB   Viterbi BER 0.140381   SYNCED/SYNCED
```

MER 4.65 is the number to compare with. Our decoder's MER at its working point is
5.8 dB and it dies at 5.1 dB. This tool now prints the same M2M4 estimate, and it
reads 2.2-2.6 dB below MER, the same offset SatDump shows. Read in one unit, the
two decoders are within about 1 dB, not 4.

## HRIT imagery (wavelet)

HRIT channels are CCSDS 122 style wavelet, and the decoder for them is now in
`third_party/decompwt` - EUMETSAT's PublicDecompWT, Apache-2.0, the same library
MSG/Meteosat ground software uses. It is fed the xRIT payload as a bit field and
returns the image packed at the declared depth (10 bits here, so 1.25 bytes per
pixel and an unpacking pass before the PNG writer).

On the 34 s ground-truth slice: 19 wavelet products, **19 PNGs, 0 unsupported**,
plus 3 stitched 2784x2784 disks. Per strip the output is 2784x464, 16-bit, values
0..1023.

The assembled disk from that capture is `h_full1.png` (2784x2784, no seams).
Two notes for whoever reads this next:

- Products whose payload arrived truncated decode to a near-black image rather
  than failing (the 11.9 channel in this slice is 1.1 KB of payload for a
  2784x464 image). That is partial data, not a decoder fault.
- Stitching now carries 16-bit strips; `SegSet` stores them widened to uint16 and
  the stretch is computed on the 10-bit range.


## BPSK polarity bug (fixed - this was costing frames everywhere)

On the 1.176 MSPS LRIT test file the demodulator's carrier loop settles on the
opposite BPSK phase, and the soft bit stream it produces is the **exact negation**
of the stream the narrow loop produces - same magnitudes, 0.0000 sign agreement
over 78 Mbit. The narrow-loop stream decoded 799/799 while the negated one
decoded 4/401, which looked like a carrier problem. It was not.

`Deframer::emit()` already normalises polarity: when a frame matches only the
inverted ASM it flips every bit, so `Cadu::bytes` is always normal and
`Cadu::inverted` is informational. `FecChain` nevertheless reacted to those
frames by negating the soft stream and resetting the Viterbi and deframer - i.e.
re-inverting frames that were already fixed, then fighting itself for the rest of
the capture. That reaction is gone.

What it was costing, before and after:

| case | before | after |
|---|---|---|
| LRIT 1.176 MSPS, 4 sps, 12 dB | 4/401 | **799/799** |
| same, 8 dB | 39/389 | 794/798 |
| same, 5.5 dB | 2/345 | 741/798 |
| LRIT 3 MSPS real capture | 4173/4173 | **4175/4175** |
| LRIT ziq capture | 4143/4143 | **4147/4147** |
| HRIT ground truth | 4785/4785 | **4799/4799** |

4175 is the number GobDump reports on the same capture, and 4799 is every frame in
the 34 s slice. Nothing was lost: `cmp_cadu` still says every payload matches the
reference, and the HRIT cliff is unchanged at 3.8/3.9 dB.

## Sync was suspected, and then ruled out

At MER 5.3 dB the deframer attempts only 886 of the 4785 frames and decodes 840 of
those. That looked like a sync-tracking gap against GobDump, which stays SYNCED
and keeps feeding RS. Two changes were tried:

- re-acquiring on a single ASM instead of two: 840 good frames stayed 840, and
  1655 garbage frames appeared;
- coasting to the predicted ASM slot after losing phase: frames attempted went
  886 -> 2495, frames decoded stayed **840**.

So the frames we fail to decode at that level are genuine RS failures, not lost
sync, and the FEC-only ladder above says the same thing. Both changes were
reverted. The decoder is at the limit of rate-1/2 K=7 + RS(255,223), and the only
honest way past it is soft-decision or erasure Reed-Solomon, not better plumbing.

## Selftest (`./elektro-ultra selftest`)

```
randomiser CCSDS sequence          ok   first byte FF (want FF)
RS clean word -> 0 corrections     ok
RS corrects 16 errors              ok
RS erasures 8 + 12 errors          ok
RS refuses 17 errors               ok
viterbi round trip                 ok   ber 0.0000 over 4032 bits
fractional delay (integer + half)  ok
```

## Bugs this measurement discipline caught

Each of these produced a decoder that "looked" fine and decoded nothing, or
produced numbers that lied:

1. Polyphase resampler prototype normalised to unity DC gain instead of unity
   per phase - output was 1/L in amplitude.
2. Fractional-delay interpolator indexed so the fractional part ran backwards -
   the timing loop could never converge.
3. Gardner loop seeding with a signed sentinel `-1e9` and comparing magnitudes -
   no candidate ever matched, so the deframer never found an ASM.
4. ASM bit table with 0x1A written as 0x1D - a 32-bit sync word that never
   matched, on a link that was decoding perfectly.
5. Costas error measured on the raw symbol instead of the derotated one - the
   loop tracked the absolute carrier phase and never settled.
6. Reed-Solomon returning corrected words in the polynomial basis while clean
   words stayed in the dual basis - invisible until the link degrades.
7. Acquisition searching +-150 kHz for the carrier line - a spurious line won and
   dragged the NCO off the signal. Then the opposite: the M-th-power search was
   limited to +-25 kHz around the band centroid, which for a 2 MHz wide QPSK
   spectrum is off by 25 kHz, so the line was never inside the window.
8. OQPSK rail alignment delaying the wrong rail by Ts/2 - the rails ended up a
   whole symbol apart, which costs ~2 bits in 8 on every frame.
9. Timing loop bandwidth normalised to the symbol rate: 0.005 is 1.5 kHz at LRIT
   and 5.8 kHz at HRIT. The wide loop closed the eye at low SNR.
10. Viterbi BER reported as an EWMA seeded at 1.0 - it reads 0.4 % on a perfectly
    clean stream and sends you hunting for a bug that is not there. It is now a
    cumulative ratio.
11. Deframer emitting frames on a single ASM hit: noise produced 211 "frames" per
    20 s, all failing RS. Two ASMs at the frame spacing are now required.
12. QPSK carrier ambiguity: a decision-directed loop settles on any of 4 phases,
    and the ASM lives inside the coded data so nothing downstream can tell. The
    FEC chain now walks the 4 hypotheses until frames appear (2 for BPSK).
13. LRIT JPEGs rejected by `stb_image` as "bad SOS": Elektro-L's onboard encoder
    writes a non-zero Ah/Al byte in a sequential scan. Ah/Al are meaningless
    there; ignoring them decodes all 30 segments.
14. xRIT completeness keyed only on the declared data-field length, which is never
    covered because each strip is one standalone JPEG. EOI (FF D9) is now the
    terminator for JPEG payloads - FF is always stuffed inside entropy data, so
    the pair can only be a marker.
