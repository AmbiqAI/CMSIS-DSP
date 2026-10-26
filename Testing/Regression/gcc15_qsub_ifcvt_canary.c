/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Canary for the GCC 15.0-15.2 __QSUB/__QADD workaround in
 * Include/arm_math_types.h (GCC PR rtl-optimization/122170). The ACLE
 * builtin under a condition, without the workaround: GCC 15.2.1 stops with an
 * internal compiler error in RTL pass ce1 at -O1 and above on any core with
 * the DSP extension; GCC 14.3.1 and 15.3.1 compile it. The runner
 * run_gcc15_qsub_ifcvt_canary.py requires the crash exactly when the
 * workaround is active for the compiler under test, so a compiler inside the
 * gate that compiles this file, or one outside it that crashes, fails the
 * run and the gate's version range is corrected. */
#include <arm_acle.h>

int canary_qsub(int x) { return x > 0 ? x : __qsub(0, x); }
