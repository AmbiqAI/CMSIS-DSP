# MVE fixed-point RFFT table guards

The standalone Cortex-M55 test calls the actual forward split kernels with
one coefficient table at a time copied immediately before an MPU-inaccessible
region. The copy includes exactly the declared library table extent. Other
sample buffers have vector-tail storage so the test isolates coefficient reads.

It covers lengths 32–8192 for Q15/Q31 and both A/B tables (36 cases). Guarded
outputs must match the same kernel using the normal library table placement.
This validates coefficient-boundary safety and placement-independent results,
not an independent mathematical FFT reference or an end-to-end transform.

Requirements: Arm GNU compiler, CMSIS-Core, Cortex_DFP, the Ethos-U
core-platform Corstone-300 linker script, and the Linux Corstone-300 FVP.
From the repository root:

```sh
python3 Testing/Regression/run_mve_rfft_table_guards.py \
  --cmsis-core /path/to/CMSIS_6/CMSIS/Core \
  --cortex-dfp /path/to/Cortex_DFP \
  --platform-linker /path/to/ethos-u-core-platform/targets/corstone-300/platform.ld \
  --fvp-root /path/to/corstone300_download \
  --build-dir /tmp/cmsis-rfft-guards
```

Expect `PASS: 36 MVE RFFT table-guard cases`. For the negative control,
save upstream's unpatched `arm_common_tables.c` and `arm_common_tables.h`
outside the checkout. Supply `--tables-source /path/to/arm_common_tables.c`
and `--tables-include /path/to/header-directory`. Expect
`FAIL: Q15 length=32 table=A guard access` and a nonzero runner exit.

The runner checks output markers, since UART shutdown alone returns zero even
for a failure. It downloads no dependencies. The common console adapter is
identical to the matrix-tail regression's adapter so the two patches can share
it when merged. Run the upstream suites separately for broader coverage.
