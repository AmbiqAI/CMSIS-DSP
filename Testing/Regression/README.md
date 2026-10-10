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

## CFFT buffer bound (final radix-4 stage)

`mve_cfft_overread.c` checks that the MVE complex FFTs (`arm_cfft_f32`,
`arm_cfft_q31`, `arm_cfft_q15`, `arm_cfft_f16`; lengths 16 to 4096, forward
and inverse) read nothing past the end of the in-place buffer: each transform
runs on a buffer that ends at an MPU-inaccessible guard, and its output is
compared with a double-precision radix-2 FFT of the same input, 72 cases.
Before the guarded runs the test measures how far each transform reads above
the buffer end (`OVERREAD` lines: the buffer is placed k words below the guard
for k = 0..64 until no fault occurs). `run_mve_cfft_overread.py` builds with
float16 enabled and expects `PASS: 72 MVE CFFT last-stage cases`.

The negative control passes upstream copies of one or more `arm_cfft_<type>.c`
through `--source`; each must report `FAIL: CFFT <type> forward length 16
touched the guard` after `OVERREAD` lines of 56 bytes (f32, Q31) or 60 bytes
(Q15, f16) at every length. Under Arm GNU 15.2.1 the numerical check fails
through that compiler's writeback-gather defect (#10); the over-read probe
still reports 0 bytes there.
