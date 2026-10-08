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

## Predicated tail loads, fixed-point kernels

`mve_tail_loads_fixed.c` applies the guarded check to the 45 Q7/Q15/Q31 and
u8/u16 kernels whose final partial block was loaded with an unpredicated
`vld1q` (or offset gather) after `vctp8q`/`vctp16q`/`vctp32q`:
`arm_abs`, `arm_negate`, `arm_offset`, `arm_scale`, `arm_shift`, `arm_add`,
`arm_sub`, `arm_mult`, `arm_dot_prod`, `arm_mse` for q7, q15 and q31;
`arm_and`, `arm_or`, `arm_xor`, `arm_not` for u8 and u16; `arm_mat_add`,
`arm_mat_sub`, `arm_mat_scale` for q15 and q31; `arm_mat_trans_q7`. Block
sizes run from 1 to two full vectors (32 for 8-bit, 16 for 16-bit, 8 for
32-bit types), matrices 1–2 rows by 1 to two vectors, the transpose 1–9 rows
by 1–3 columns: 923 cases. Each case compares the guarded run with a run on
readable nonzero padding (identical output) and with a double-precision
reference (exact for add, sub, abs, negate, offset, shift and the bitwise
kernels; within 2 LSB for mult and scale, n + 2 LSB for dot products and
4 LSB for mse); output canaries detect boundary writes.

`run_mve_tail_loads.py --group fixed` expects
`PASS: 923 MVE fixed-point tail-load cases`; the negative control through
`--source` must report `FAIL: <kernel> tail access n=1`.
