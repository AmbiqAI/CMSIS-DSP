/* Compiled into the host reference archive with the library's own flags: fails the build if anything
   selected a code path other than the plain C one, first as given, before any header acts on it. */
#if defined(ARM_MATH_DSP) || defined(ARM_MATH_MVEI) || defined(ARM_MATH_MVEF) || defined(ARM_MATH_HELIUM) || \
    defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE) || defined(ARM_MATH_NEON) ||          \
    defined(ARM_MATH_NEON_EXPERIMENTAL) || defined(ARM_MATH_NEON_FLOAT16) || defined(ARM_MATH_LOOPUNROLL) ||  \
    defined(ARM_MATH_ROUNDING) || defined(ARM_MATH_MATRIX_CHECK) || defined(__ARM_FEATURE_DSP) ||            \
    defined(__ARM_FEATURE_MVE)
#error "the CMSIS-DSP host reference must be built from the plain C code only"
#endif
/* Again as Arm's headers derive them; a header such as arm_mve.h may already have stopped the compile. */
#include "arm_math.h"
#include "arm_math_f16.h"

#if defined(ARM_MATH_DSP) || defined(ARM_MATH_MVEI) || defined(ARM_MATH_MVEF) || defined(ARM_MATH_HELIUM) || \
    defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE) || defined(ARM_MATH_NEON) ||          \
    defined(ARM_MATH_NEON_EXPERIMENTAL) || defined(ARM_MATH_NEON_FLOAT16) || defined(ARM_MATH_LOOPUNROLL) ||  \
    defined(ARM_MATH_ROUNDING) || defined(ARM_MATH_MATRIX_CHECK) || defined(__ARM_FEATURE_DSP) ||            \
    defined(__ARM_FEATURE_MVE)
#error "the CMSIS-DSP host reference must be built from the plain C code only"
#endif
#if !defined(ARM_FLOAT16_SUPPORTED)
#error "the CMSIS-DSP host reference needs float16_t, so every f16 kernel has a reference"
#endif

/* Marks an archive as the host reference. */
const char cmsis_dsp_host_reference[] = "cmsis-dsp host reference";
