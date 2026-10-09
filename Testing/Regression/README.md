# Standalone MVE matrix-tail regression

`mve_matvec_tail.c` runs the handwritten f32 matrix-vector kernel with an
MPU-inaccessible region immediately after the matrix. It checks 63 shapes:
1–7 rows and 1–9 columns, covering the 4/2/1-row paths, all vector tail lengths,
and full-vector cases. Results are compared with double-precision accumulation
of exactly representable inputs; output canaries detect boundary writes.

The test intentionally provides no matrix padding. Upstream's general
vector-buffer documentation allows three words of readable padding; this test
qualifies the stronger boundary behavior of the proposed predicated loads.
It does not change padding requirements for other functions or measure speed.

Requirements: Arm GNU compiler, CMSIS-Core headers, Cortex_DFP, the Ethos-U
core-platform Corstone-300 `platform.ld`, and the Linux Corstone-300 FVP package.
No dependencies are downloaded by the runner. From the repository root:

```sh
python3 Testing/Regression/run_mve_matvec_tail.py \
  --cmsis-core /path/to/CMSIS_6/CMSIS/Core \
  --cortex-dfp /path/to/Cortex_DFP \
  --platform-linker /path/to/ethos-u-core-platform/targets/corstone-300/platform.ld \
  --fvp-root /path/to/corstone300_download \
  --build-dir /tmp/cmsis-matvec-regression
```

For the negative control, save the unpatched `arm_mat_vec_mult_f32.c` outside
the checkout and pass its absolute path through `--matrix-source`. It must
report `FAIL: matrix tail access rows=1 cols=1`; the patched source must report
`PASS: 63 MVE matrix-tail cases`. The runner checks the output as well as the
model status because UART-triggered shutdown alone does not encode pass/fail.
This focused regression complements, rather than replaces, the upstream suites.

## Fixed-point companion

`mve_matvec_tail_fixed.c` applies the same MPU-guarded check to the Q7, Q15,
and Q31 kernels: 1–7 rows and 1–17 columns per datatype, 357 cases in all.
Each case compares the guarded run with a run on readable nonzero padding
(identical output) and with a scalar 64-bit accumulation (within one LSB).
The runner `run_mve_matvec_tail_fixed.py` takes the same dependency options
as the f32 runner. `--datatype q7|q15|q31` compiles and runs one kernel only
and expects `PASS: 119 MVE fixed-point matrix-tail cases`; the default `all`
expects 357.

The negative control is per datatype, because the first fault ends the run:
save the unpatched `arm_mat_vec_mult_<type>.c` outside the checkout and pass

```sh
python3 Testing/Regression/run_mve_matvec_tail_fixed.py ... \
  --datatype q15 --matrix-source /path/to/upstream/arm_mat_vec_mult_q15.c
```

which must report `FAIL: Q15 matrix tail access rows=1 cols=1`. Repeat for
`q7` and `q31`.

## f32 FIR decimator state

`mve_fir_decimate_f32_state.c` runs `arm_fir_decimate_f32` with its state buffer, of the documented length `numTaps + blockSize − 1`, ending at an MPU-inaccessible region: 1–20 taps, decimation factors 1–5, and 1–9 outputs per call (covering the four-output loop and the single-output loop), two consecutive calls per case, 900 cases. Each case also runs on a state buffer followed by readable +Inf padding; both outputs must equal the double-precision reference bit for bit (inputs and coefficients are chosen so every partial sum is exact in f32), with output canaries. The test covers both over-reads the kernel had: the state loads of the partial final tap block, and the load of the final state copy.

`run_mve_fir_decimate_f32_state.py` takes the options of the f16 matrix-vector runner and expects `PASS: 900 MVE f32 decimator state cases`. The negative control passes the unpatched `arm_fir_decimate_f32.c` through `--decimate-source` and must report `FAIL: decimator state access taps=1 M=1 outputs=1` (the tap remainder); the same source with only the state-copy load unpredicated reports `taps=2 M=1 outputs=1`.

## f32 FIR decimator equivalence

`mve_fir_decimate_f32_equivalence.c` is an equivalence regression for changes to `arm_fir_decimate_f32` meant to preserve output: it links the checkout's kernel and a reference copy of `arm_fir_decimate_f32.c` from another revision, compiled with `-Darm_fir_decimate_f32=arm_fir_decimate_f32_ref`, and compares outputs and the retained state bit for bit on pseudo-random data: 1–40 taps × decimation factor 1–8 × 1–12 outputs per call, two calls per case, 3,840 cases. `run_mve_fir_decimate_f32_equivalence.py --reference-source <file>` takes the options of the other runners, compiles without `-ffast-math`, and expects `PASS: 3840 MVE f32 decimator equivalence cases`. A reference that sums the four lanes pairwise instead of in order reports `FAIL: decimator output differs from the reference taps=4 M=1 outputs=4` under Arm GNU.
