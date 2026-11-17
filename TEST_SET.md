# The sdr_test_setup share - what it gives us

`https://cloud.a-where.com/share/qoAYAMaH` (author: seler1500). Four directories:
`cadus`, `flowgraphs`, `modulated basebands`, `real basebands`.

This is the ground truth we did not have. Everything below is measured here.

## What is in it

| item | size | what it is |
|---|---|---|
| `cadus/elektro_hrit.cadu` | 27 992 064 B | **27 336 real HRIT CADUs** (1024 B each) |
| `cadus/elektro_lrit.cadu` | 4 275 200 B | 4 175 real LRIT CADUs |
| `cadus/gk2a_hrit_by_thedag.cadu` | 300 MB | GK-2A HRIT frames (other satellite) |
| `cadus/goes-16_hrit_by_nate.cadu` | 169 MB | GOES-16 HRIT frames (other satellite) |
| `modulated basebands/elektro_hrit_qpsk.wav16` | 3.58 GB | the CADUs above, modulated QPSK |
| `modulated basebands/elektro_hrit_oqpsk.wav16` | 3.58 GB | same, OQPSK variant |
| `modulated basebands/elektro_lrit.wav16` | 1.09 GB | LRIT CADUs, modulated |
| `modulated basebands/gk2a_hrit.wav16` | 19.2 GB | GK-2A |
| `modulated basebands/goes_hrit.wav16` | 21.7 GB | GOES |
| `real basebands/real_metop.cs16` | 9.68 GB | real MetOp (not Elektro) |
| `flowgraphs/*.grc` | 10-16 KB | the GNU Radio graphs that made them |

## What the flowgraph says (authoritative)

`elektro_hrit_cadu_to_wav16.grc`, read from its `connections:` list:

```
file .cadu -> unpack bits -> keep 8160 of 8192 (offset 32)  [drops the ASM]
           -> additive scrambler (mask 0xA9, count 8160, seed 0xFF)
           -> stream mux with the 32 ASM bits   <- ASM goes in BEFORE the encoder
           -> cc_encoder (k=7, rate 1/2, polys 79/109 = 171/133 octal, streaming)
           -> repack 1->2 -> chunks_to_symbols [1+1j, -1+1j, 1-1j, -1-1j]
           -> RRC alpha 0.5, ntaps 11*sps+1 -> channel model (noise 0.05) -> wav16
```

Two things this settles:

1. **The ASM is inside the convolutionally coded stream**, which is the
   architecture elektro-ultra already uses (decode first, then deframe). It is
   not the textbook "ASM attached after encoding".
2. The randomiser covers 8160 bits *after* the 32-bit ASM, exactly as our
   derandomiser does.

The WAV is PCM16, 2 channels, header rate 4 600 000 = 4 samples per 1.15 MBd
symbol. 194.7 s of signal, 27 336 CADUs at 140/s.

## Result 1 - the HRIT chain is exact

Decoding 11.4 s of `elektro_hrit_qpsk.wav16` with `--mod qpsk`:

```
demod : snr 22.7 dB  evm 7.3%  carrier +0.0 Hz  clock -0.0 ppm
lock  : viterbi locked (ber 0.000)  deframer locked (asm 1.00)
frames: 139.1/s  good 1585/1585  rs fixed 0  failed 0
```

Then, byte for byte against the reference CADU file (RS-decoded independently by
`cmp_cadu`, a separate program):

```
reference: 27336 cadus, asm ok 27336, rs fail 0
compare  : 1413820 bytes, 0 differ (identical)
hash ref 3719a99a6c2228a0  ours 3719a99a6c2228a0
```

1585/1585 frames, and not one bit wrong in 1.4 MB of payload. That is the proof
the whole HRIT chain - demod, Viterbi, deframe, derandomise, RS - is correct.

## Result 2 - transport works, products do not

The first 4200 CADUs of the file are idle fill; data starts at CADU 4200
(t ~ 30 s). Decoding around 118 s gives:

```
frames   : 1585/1585 good
transport: 165 packets (0 bad crc), 4 xRIT files, 0 complete
products : 3 images -> 0 png, 1 raw, 3 unsupported
```

The three "unsupported" images are compression code **1 = CCSDS 122 wavelet**:

```
H-000-GOMS3_-GOMS3_4_____-03_8_076E-000001___-202601031500-C uses compression 1 (wavelet), no decoder yet
```

So HRIT images are wavelet, not JPEG (JPEG is LRIT only). Our product stage can
recognise them, reassemble them and dump the raw `.wt` payloads, and stops there.
GobDump ships EUMETSAT's DecompWT for this.

## Result 3 - the constellation preset is wrong for this file

`--type hrit` and even `--type auto` both pick OQPSK, and OQPSK on a QPSK signal
gives EVM 39.5 % and zero frames:

| mod | EVM | frames |
|---|---|---|
| oqpsk | 39.5 % | 0 |
| qpsk | 7.3 % | **1585/1585** |
| bpsk | 87.2 % | 0 |

`probe` reports "qpsk/oqpsk" and the preset resolves that to OQPSK. Both the
real capture and this test file prefer QPSK.

## What this changes

- HRIT is no longer "unverified". The chain is provably exact; what was missing
  was a usable capture, and that is now here (27 336 CADUs, 194 s).
- The next real blocker for HRIT products is a **CCSDS 122 wavelet decoder**.
- Constellation selection must try both when the first choice decodes nothing.
