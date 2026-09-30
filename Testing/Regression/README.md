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
