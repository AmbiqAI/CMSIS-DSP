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

## FIR f32 state-bound regression

`mve_fir_f32_state_bound.c` places the `arm_fir_f32` state buffer against an
MPU guard for 1–20 taps and 1–12 samples per block (240 cases, covering the 1–4, 5–8, 9–12, 13–16, and 17+ tap paths) and, after a
recovered MemManage fault, grows the buffer one element at a time until the
kernel runs without touching the guard. It records for each case how many
elements beyond the documented `numTaps + 2 * blockSize - 1` were touched
and whether any were stored to. The pass condition is no stores past the
documented length and no reads past `4 * ceil(numTaps / 4) + 2 * blockSize - 1`,
the rounding the coefficient array already needs; outputs are checked against
a double-precision reference in every case.

```sh
python3 Testing/Regression/run_mve_fir_f32_state_bound.py <dependency options> \
  --build-dir /path/to/build
```

The input block ends at a second guard, and the initializer must have
cleared everything the kernel reads. For the negative control, pass the
unpatched `arm_fir_f32.c` through `--fir-source`; its residual input copy
reads past the input block, which no amount of state padding cures, so it
must report `FAIL: numTaps=1 blockSize=1 still faults with 16 extra elements`.

## Q31 CFFT guard regression (compiler check)

`mve_cfft_q31_guard.c` runs the MVE Q31 complex FFT, forward and inverse,
for lengths 16 to 1024 on a buffer that starts immediately after an
MPU-inaccessible guard, with canary padding above it, and checks the result
against a double-precision DFT (14 cases). The final radix-4 stage gathers
with `vldrwq_gather_base_wb_s32` and then addresses relative to the
written-back base; a compiler that issues those accesses from the
pre-writeback base touches 64 bytes below each block. Arm GNU Toolchain
15.2.Rel1 does so at every optimization level (`FAIL: CFFT Q31 forward
length 16 touched the guard`); 14.x passes. The runner takes `--compiler`
and `--opt O2|O3` so both toolchains can be exercised:

```sh
python3 Testing/Regression/run_mve_cfft_q31_guard.py <dependency options> \
  --compiler /path/to/arm-gnu-15.2/bin/arm-none-eabi-gcc --opt O2 --build-dir /path/to/build
```

Under a correct compiler the upstream kernel reads one element past the
buffer for the 16-point transform, which is why the upper side is padding
rather than a second guard.

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

## Predicated tail loads, f16 kernels

`mve_tail_loads_f16.c` applies the guarded check to the fifteen f16 kernels
whose final partial block was loaded with an unpredicated `vld1q` (or
unpredicated gather) after `vctp16q`: `arm_abs_f16`, `arm_negate_f16`,
`arm_offset_f16`, `arm_scale_f16`, `arm_add_f16`, `arm_sub_f16`,
`arm_mult_f16`, `arm_dot_prod_f16`, `arm_mse_f16`, `arm_absmin_f16`,
`arm_cmplx_mult_real_f16`, `arm_q15_to_f16`, `arm_mat_add_f16`,
`arm_mat_sub_f16`, `arm_mat_scale_f16`. Block sizes 1 to 16 and matrices of
one and two rows by 1 to 16 columns, 288 cases. Inputs are exact in half
precision and small enough that every result except the mean square error is
exact; mse is checked within 2 percent.

`run_mve_tail_loads.py --group f16` builds without `DISABLEFLOAT16` and
expects `PASS: 288 MVE f16 tail-load cases`; Arm GNU 14.3.1 and ATfE 22.1.0
both compile this group. The negative control through `--source` must report
`FAIL: <kernel> tail access n=1`.

## Predicated tail loads, remaining kernels

`mve_tail_loads_remainder.c` covers the five kernels that the f32, fixed-point
and f16 tail-load groups left unchanged. `arm_cmplx_mag_f16`,
`arm_cmplx_mag_squared_f16` and `arm_float_to_f16` loaded their final partial
block with `vld2q`, which has no predicated form; they now gather the
de-interleaved elements under the tail predicate. `arm_mat_add_f32` and
`arm_mat_sub_f32` compute a block predicate in every iteration and now load
under it. Block sizes 1 to 16 for the three vector kernels, one and two rows by
1 to 16 columns for the matrices: 112 cases. `run_mve_tail_loads.py --group
remainder` builds with float16 enabled and expects
`PASS: 112 MVE remainder tail-load cases`.

The negative control through `--source` must report `FAIL: <kernel> tail
access n=1` for the two complex kernels under either compiler and for
`arm_float_to_f16` under ATfE; upstream compiles that kernel's MVE path only
under compilers other than GCC (`ARM_DSP_BUILT_WITH_GCC`). The matrix kernels
pass the negative control at `O1` to `O3` because Arm GNU 14.3 and ATfE 22.1
compile their predicated `do`/`while` bodies to tail-predicated
(`dlstp`/`letp`) loops at those levels, and fail it at `O0` under both
compilers, where the unpredicated load is emitted as written; the runner's
`O0` and `O1` levels exist for such controls.
