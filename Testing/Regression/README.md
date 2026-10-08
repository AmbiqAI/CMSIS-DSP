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

## Predicated tail loads, f32 and u32 kernels

`mve_tail_loads_f32.c` runs sixteen f32/u32 vector kernels whose final partial
block was loaded with an unpredicated `vld1q` after `vctp32q` (reading up to
three elements past each source), plus `arm_mat_add_f32` and `arm_mat_sub_f32`
for coverage, with every source buffer ending at an MPU-inaccessible region:
`arm_abs_f32`, `arm_negate_f32`, `arm_offset_f32`, `arm_scale_f32`,
`arm_add_f32`, `arm_sub_f32`, `arm_mult_f32`, `arm_and_u32`, `arm_or_u32`,
`arm_xor_u32`, `arm_not_u32`, `arm_dot_prod_f32`, `arm_accumulate_f32`,
`arm_mse_f32`, `arm_absmin_f32`, `arm_mat_scale_f32`. Block sizes 1 to 8 and
matrices of one and two rows by 1 to 8 columns, 168 cases, compared with
double-precision references; output canaries detect boundary writes.

`run_mve_tail_loads.py --group f32` takes the dependency options of the other
runners plus `--compiler` (Arm GNU `arm-none-eabi-gcc`, or an ATfE `clang`,
compiled with `--target=arm-none-eabihf`), `--linker` (an Arm GNU driver, used
for every build) and `--opt O2|O3`, and expects
`PASS: 168 MVE f32/u32 tail-load cases`. For the negative control, save one
unpatched kernel outside the checkout and pass it through `--source`; the run
must report `FAIL: <kernel> tail access n=1`. The two matrix kernels left
unchanged pass this control, because Arm GNU 14.3 and ATfE 22.1 compile their
do/while bodies to `dlstp`/`letp` loops at `-O2` and `-O3`, which predicate the
loads implicitly.
