#!/bin/bash
# FEC sweep on ground truth: add noise to the clean HRIT test baseband and record
# what the decoder does at each Es/N0. Run from anywhere:
#   IQ=/path/hrit_clean.cs16 bash tools/fec-sweep.sh
# The first row is the baseline; the last two are controls with the default
# carrier loop. Expect 4785/4785 down to about 6 dB and nothing below 5 dB -
# that cliff belongs to rate-1/2 K=7 + RS(255,223), not to this code.
EU=${EU:-/opt/data/projects/elektro-ultra/elektro-ultra}
IQ=${IQ:-/tmp/hrit_clean.cs16}
CSV=${CSV:-/tmp/fec_sweep.csv}
echo "target,pll_bw,measured_snr,evm,good,total,rs_fixed,rs_failed,ber,secs" > $CSV
run() {
  local snr=$1 pll=$2 tag=$3
  $EU noisify -i "$IQ" -o /tmp/n2.cs16 --rate 4600000 --baud 1150000 --snr $snr --seed 7 >/dev/null 2>&1
  out=$($EU decode -i /tmp/n2.cs16 --rate 4600000 --type hrit --mod qpsk --images off \
        --pll-bw $pll --pll-bw-acq $pll -o /tmp/sw2_$tag 2>&1)
  g() { echo "$out" | grep -oP "$1" | head -1; }
  echo "$snr,$pll,$(g 'snr \K[0-9.+-]+'),$(g 'evm \K[0-9.]+'),$(g 'good \K[0-9]+'),$(g 'good [0-9]+/\K[0-9]+'),$(g 'rs fixed \K[0-9]+'),$(g 'failed \K[0-9]+'),$(g 'ber \K[0-9.]+'),$(g 'IQ in \K[0-9.]+')" >> $CSV
  tail -1 $CSV
}
for s in 8 7 6 5 4 3 2 1; do run $s 0.00001 p$s; done
run 4 0.0015 c4
run 3 0.0015 c3
echo SWEEP2_DONE
