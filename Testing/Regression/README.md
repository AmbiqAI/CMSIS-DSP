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
matrices of one and two rows by 1 to 8 columns, 168 cases (184 with the
statistics kernels below), compared with
double-precision references; output canaries detect boundary writes.

`run_mve_tail_loads.py --group f32` takes the dependency options of the other
runners plus `--compiler` (Arm GNU `arm-none-eabi-gcc`, or an ATfE `clang`,
compiled with `--target=arm-none-eabihf`), `--linker` (an Arm GNU driver, used
for every build) and `--opt O2|O3`, and expects
`PASS: 184 MVE f32/u32 tail-load cases`. For the negative control, save one
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
by 1–3 columns: 923 cases (1163 with the statistics kernels below). Each case compares the guarded run with a run on
readable nonzero padding (identical output) and with a double-precision
reference (exact for add, sub, abs, negate, offset, shift and the bitwise
kernels; within 2 LSB for mult and scale, n + 2 LSB for dot products and
4 LSB for mse); output canaries detect boundary writes.

`run_mve_tail_loads.py --group fixed` expects
`PASS: 1163 MVE fixed-point tail-load cases`; the negative control through
`--source` must report `FAIL: <kernel> tail access n=1`.

## Predicated tail loads, f16 kernels

`mve_tail_loads_f16.c` applies the guarded check to the fifteen f16 kernels
whose final partial block was loaded with an unpredicated `vld1q` (or
unpredicated gather) after `vctp16q`: `arm_abs_f16`, `arm_negate_f16`,
`arm_offset_f16`, `arm_scale_f16`, `arm_add_f16`, `arm_sub_f16`,
`arm_mult_f16`, `arm_dot_prod_f16`, `arm_mse_f16`, `arm_absmin_f16`,
`arm_cmplx_mult_real_f16`, `arm_q15_to_f16`, `arm_mat_add_f16`,
`arm_mat_sub_f16`, `arm_mat_scale_f16`. Block sizes 1 to 16 and matrices of
one and two rows by 1 to 16 columns, 288 cases (352 with the statistics
kernels below). Inputs are exact in half
precision and small enough that every result except the mean square error is
exact; mse is checked within 2 percent.

`run_mve_tail_loads.py --group f16` builds without `DISABLEFLOAT16` and
expects `PASS: 352 MVE f16 tail-load cases`; Arm GNU 14.3.1 and ATfE 22.1.0
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

## Predicated tail loads, statistics kernels

Eighteen statistics kernels loaded their final partial block with an
unpredicated `vld1q`, `vldrwq` or `vldrhq` and applied the tail predicate only
to the comparison that follows, so they read up to one vector minus one
element past the source: `arm_absmin`, `arm_absmin_no_idx`, `arm_min_no_idx`
and `arm_max_no_idx` for q7, q15 and q31; `arm_absmin_no_idx_f32`,
`arm_absmax_no_idx_f32`; `arm_absmin_no_idx_f16`, `arm_absmax_f16`,
`arm_absmax_no_idx_f16` and `arm_min_f16`. Each now computes the tail
predicate first and loads under it. The existing groups cover them:
`--group fixed` the twelve fixed-point kernels at 1 to two full vectors and,
for q7, at 255, 256, 257 and 300 elements (the block path above 256 elements
of `arm_absmin_q7`), 240 cases; `--group f32` the two f32 kernels at 1 to 8
elements, 16 cases; `--group f16` the four f16 kernels at 1 to 16 elements,
64 cases. The value is compared with a double-precision reference; for
`arm_absmin_q7/q15/q31` the index must point to an element with that absolute
value and equal the index of the run on padded data, and for `arm_absmax_f16`
and `arm_min_f16` (distinct magnitudes) it must be the reference index. Each
unchanged kernel passed through `--source` must report
`FAIL: <kernel> tail access n=1`.

## CFFT buffer bound (final radix-4 stage)

`mve_cfft_overread.c` checks that the MVE complex FFTs (`arm_cfft_f32`,
`arm_cfft_q31`, `arm_cfft_q15`, `arm_cfft_f16`; lengths 16 to 4096, forward
and inverse) read nothing past the end of the in-place buffer: each transform
runs on a buffer that ends at an MPU-inaccessible guard, and its output is
compared with a double-precision radix-2 FFT of the same input, 72 cases.
Before the guarded runs the test measures how far each transform reads above
the buffer end (`OVERREAD` lines: the buffer is placed k words below the guard
for k = 0..64 until no fault occurs). The guard spans that 256-byte probe
range plus one 16-byte vector access, rounded to the 32-byte MPU granule (288
bytes), so a read anywhere in it faults however sparse. f32 and f16 outputs
whose exponent bits are all ones (infinity or NaN) fail before the error is
computed; the bit test holds under `-ffast-math`. `run_mve_cfft_overread.py`
builds with float16 enabled and expects `PASS: 72 MVE CFFT last-stage cases`.

`--control` builds one of the test's own negative controls, and the run then
succeeds only if the control fails as stated: `sparse-read-32` and
`sparse-read-256` add one volatile 32-bit read 32 or 256 bytes past the f32
buffer after the transform (`FAIL: CFFT f32 forward length 16 touched the
guard`, with `OVERREAD` 36 bytes, or beyond the probe range, for f32);
`nan-f32` and `nan-f16` write a quiet NaN into the first output (`FAIL: CFFT
<type> forward length 16 output 0 is not finite`).

The negative control passes upstream copies of one or more `arm_cfft_<type>.c`
through `--source`; each must report `FAIL: CFFT <type> forward length 16
touched the guard` after `OVERREAD` lines of 56 bytes (f32, Q31) or 60 bytes
(Q15, f16) at every length. Under Arm GNU 15.2.1 the numerical check fails
through that compiler's writeback-gather defect (#10); the over-read probe
still reports 0 bytes there.

## f16 companion

`mve_matvec_tail_f16.c` applies the same MPU-guarded check to the f16 kernel: 1–7 rows and 1–17 columns, 119 cases, covering the 4/2/1-row paths and every tail length of an eight-lane vector. Each case runs the kernel on a matrix followed by readable padding that holds f16 +Inf, then on the same matrix ending at the guard; both outputs must equal the double-precision reference bit for bit (the inputs are multiples of 1/8 and 1/4, so every partial sum is exact in f16). A read of the padding multiplies Inf by a zeroed vector lane and gives NaN, so the padded run detects an over-read on a target without an MPU as well.

`run_mve_matvec_tail_f16.py` takes the same dependency options as the f32 runner, plus `--compiler` (Arm GNU, or ATfE clang, which compiles with `--target=arm-none-eabihf` and links with Arm GNU) and `--opt O0|O1|O2|O3`; it expects `PASS: 119 MVE f16 matrix-tail cases`. The negative control passes the unpatched `arm_mat_vec_mult_f16.c` through `--matrix-source` and must report `FAIL: f16 matrix tail access rows=1 cols=1`.

## f32 FIR decimator state

`mve_fir_decimate_f32_state.c` runs `arm_fir_decimate_f32` with its state buffer, of the documented length `numTaps + blockSize − 1`, ending at an MPU-inaccessible region: 1–20 taps, decimation factors 1–5, and 1–9 outputs per call (covering the four-output loop and the single-output loop), two consecutive calls per case, 900 cases. Each case also runs on a state buffer followed by readable +Inf padding; both outputs must equal the double-precision reference bit for bit (inputs and coefficients are chosen so every partial sum is exact in f32), with output canaries. The test covers both over-reads the kernel had: the state loads of the partial final tap block, and the load of the final state copy.

`run_mve_fir_decimate_f32_state.py` takes the options of the f16 matrix-vector runner and expects `PASS: 900 MVE f32 decimator state cases`. The negative control passes the unpatched `arm_fir_decimate_f32.c` through `--decimate-source` and must report `FAIL: decimator state access taps=1 M=1 outputs=1` (the tap remainder); the same source with only the state-copy load unpredicated reports `taps=2 M=1 outputs=1`.

## Python wrapper commit hash

`test_pythonwrapper_commit_hash.py` checks the commit that the Python wrapper
reports as `cmsisdsp.commit_hash`, from fresh clones of one commit of this
repository (`--commit`, default `HEAD`): a clean checkout reports the commit, a
modified tracked file adds `-dirty`, untracked and ignored files do not, a
tree without `.git` and without `cmsisdsp/_build_info.py` writes no file, and
the checkout's sdist, unpacked inside an unrelated Git repository, keeps the
commit it carries through `setup.py --name`. With `--wheel` it also builds a
wheel from that nested sdist (which compiles the wrapper), checks the commit
in the wheel, installs it into a directory on `PYTHONPATH`, and checks
`cmsisdsp.commit_hash`, an `arm_add_f32` call, and the fallback literal once
the installed `_build_info.py` is removed. The interpreter needs numpy,
setuptools and `build`; it expects `PASS: Python wrapper commit-hash cases`.
At a commit whose `setup.py` asks Git from the unpacked sdist, the nested case
reports the parent repository's commit and fails.

## Q15 and Q31 FIR decimator state and input

`mve_fir_decimate_fixed_state.c` applies the decimator check to `arm_fir_decimate_q15` and `arm_fir_decimate_q31`: 1–20 taps, decimation factors 1–5 and 1–9 outputs per call, two consecutive calls per case, 900 cases per datatype. Each case runs three times: with every buffer in readable memory followed by nonzero padding, with the state buffer (`numTaps + blockSize − 1` samples) ending at an MPU-inaccessible region, and with the input block ending there. The three outputs must be identical and within one LSB of a 64-bit reference. It covers the over-reads both kernels had in the partial final tap block of the four-output loop and in the final state copy, and the Q15 kernel's over-read of the input when `4·M` is not a multiple of eight.

`run_mve_fir_decimate_fixed_state.py` takes the options of the f32 decimator runner plus `--datatype q15|q31|all`, and expects `PASS: 900 MVE fixed-point decimator state cases` per datatype. The negative control passes the unpatched `arm_fir_decimate_<type>.c` through `--decimate-source` with `--datatype`; both report `FAIL: Q<n> decimator state access taps=1 M=1 outputs=4` (tap block). The patched sources with only the state-copy load unpredicated report `taps=2 M=1 outputs=1`, and the Q15 source with only the input-copy load unpredicated reports `FAIL: Q15 decimator input access taps=1 M=1 outputs=4`.

## Guard width, nonfinite outputs and negative controls (guarded regressions)

`regression_checks.h` is shared by the guarded regressions above:
`mve_matvec_tail.c`, `mve_matvec_tail_fixed.c`, `mve_rfft_table_guards.c`,
`mve_fir_f32_state_bound.c`, `mve_cfft_q31_guard.c` and the four
`mve_tail_loads_*.c` tests. Each MPU guard covers the extent the test guards
plus one 16-byte vector access, rounded up to the 32-byte MPU granule
(`REGRESSION_GUARD_BYTES`), so a read anywhere within that distance of a
buffer faults however sparse; a 32-byte guard let a read 32 or more bytes
away reach mapped memory. The extents: the largest matrix for the two
matrix-vector tests (7 x 9 f32, 7 x 17 Q31), the largest coefficient table
for the RFFT table test (8194 Q31 elements), the largest probed state or
input for the FIR state test, one full buffer below the buffer for the Q31
CFFT test, and the 512-byte source block for the tail-load tests.
`mve_matvec_tail.c` and `mve_fir_f32_state_bound.c` now reject f32 outputs
with an all-ones exponent (infinity or NaN) before the tolerance test, by a
bit test that holds under `-ffast-math`; the f32 and f16 tail-load tests
already did.

Each runner takes `--control`, which builds a negative control; the run then
succeeds only if the test reports a failure: `sparse-read-32` reads 32 bits
32 bytes past the end of the first guarded buffer (below its start for the
Q31 CFFT test) after the kernel call, `sparse-read-far` the same at the far
end of the guard, and `nan` (the float tests) writes a quiet NaN into the
first checked output. With 32-byte guards, `sparse-read-32` passes every
one of these tests, and `nan` passes the matrix-vector and FIR state tests.
