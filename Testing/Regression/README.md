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
