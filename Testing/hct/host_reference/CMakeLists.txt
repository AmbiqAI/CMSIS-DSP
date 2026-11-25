# CMSIS-DSP built for the host CPU from its plain C code only: no Helium, MVE, Neon, DSP extension or loop
# unrolling. helia-core-tester records test goldens from it.
#
#   cmake -S Testing/hct/host_reference -B build/host-ref -DCMSISDSP_HOST_REFERENCE=ON
#   cmake --build build/host-ref
#
# writes build/host-ref/cmsis_dsp_host_reference.json (schema hct/host-reference/1): the archive and what a
# caller compiles and links with. Every refusal is a FATAL_ERROR: a reference built with fast math or with a
# vector path selected would produce wrong goldens without any sign of it.
cmake_minimum_required(VERSION 3.23)
project(cmsis_dsp_host_reference LANGUAGES C)

get_filename_component(DSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(HOST_REFERENCE_SCHEMA "hct/host-reference/1")
set(HOST_REFERENCE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/cmsis_dsp_host_reference.json")
set(HOST_REFERENCE_OPTIONS -O2 -ffp-contract=off -fno-fast-math)
# The macros that select a code path other than the plain C one, and the Cortex-M features that imply them.
# A host's own __ARM_NEON selects nothing: every Neon path is behind ARM_MATH_NEON.
set(HOST_REFERENCE_PATH_MACROS
    ARM_MATH_DSP ARM_MATH_MVEI ARM_MATH_MVEF ARM_MATH_HELIUM ARM_MATH_MVE_FLOAT16 ARM_MATH_AUTOVECTORIZE
    ARM_MATH_NEON ARM_MATH_NEON_EXPERIMENTAL ARM_MATH_NEON_FLOAT16 ARM_MATH_LOOPUNROLL
    __ARM_FEATURE_DSP __ARM_FEATURE_MVE)
# Flags that let the compiler change IEEE results (signed zeros, NaN and infinity handling, rounding of
# intermediates, reassociation, approximate functions); later -fno-fast-math does not undo all of them.
set(HOST_REFERENCE_UNSAFE_FP
    "(^|[ ;])(-Ofast|-ffast-math|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math|-ffinite-math-only|-ffp-contract=(fast|on)|-fno-signed-zeros|-fno-honor-nans|-fno-honor-infinities|-fexcess-precision=fast|-fcx-limited-range|-fcx-fortran-rules|-fapprox-func|-ffp-model=(fast|aggressive)|-mrecip(=[^ ;]*)?|-mfpmath=[^ ;]*387[^ ;]*|-fsingle-precision-constant|-ffp-eval-method=(double|extended))([ ;]|$)")
# Source/ options that change what the reference computes or builds; each must be OFF. ROUNDING and
# MATRIXCHECK change results and status codes, and the manifest cannot say they were set.
set(HOST_REFERENCE_OFF_OPTIONS
    HELIUM HELIUMEXPERIMENTAL MVEF MVEI MVEFLOAT16 NEON NEONEXPERIMENTAL AUTOVECTORIZE LOOPUNROLL DISABLEFLOAT16
    ARMAC5 FASTBUILD ROUNDING MATRIXCHECK)

function(host_reference_fail)
  string(JOIN "" reason ${ARGV})
  message(FATAL_ERROR "CMSIS-DSP host reference: ${reason}")
endfunction()

option(CMSISDSP_HOST_REFERENCE "Build the CMSIS-DSP host reference; this project builds nothing else" ON)
if(NOT CMSISDSP_HOST_REFERENCE)
  host_reference_fail("this project only builds the host reference; configure it with CMSISDSP_HOST_REFERENCE=ON")
endif()
if(CMAKE_CROSSCOMPILING)
  host_reference_fail(
    "it is built for the host CPU; configure it without a cross toolchain (CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  host_reference_fail("needs GCC or Clang for its floating-point flags (got ${CMAKE_C_COMPILER_ID})")
endif()
get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(multi_config)
  host_reference_fail("needs a single-configuration generator such as Ninja or Unix Makefiles; ${CMAKE_GENERATOR} "
                      "builds one archive per configuration, and the manifest names one")
endif()
foreach(name IN LISTS HOST_REFERENCE_OFF_OPTIONS)
  if(DEFINED ${name} AND ${name})
    host_reference_fail("${name}=${${name}} changes what the reference computes or builds; leave it OFF")
  endif()
  set(${name} OFF CACHE BOOL "" FORCE)
endforeach()

# CC="cc -flag" puts the flag in CMAKE_C_COMPILER_ARG1.
set(flag_vars CMAKE_C_COMPILER_ARG1 CMAKE_C_FLAGS)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
  string(TOUPPER "${config}" config)
  list(APPEND flag_vars CMAKE_C_FLAGS_${config})
endforeach()
list(REMOVE_DUPLICATES flag_vars)
foreach(var IN LISTS flag_vars)
  if("${${var}}" MATCHES "${HOST_REFERENCE_UNSAFE_FP}")
    host_reference_fail("${var} contains '${CMAKE_MATCH_2}', which changes floating-point results; the reference "
                        "is built with ${HOST_REFERENCE_OPTIONS}")
  endif()
endforeach()
foreach(path IN ITEMS "${DSP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_C_COMPILER}")
  if(path MATCHES "[\"\\\\]")
    host_reference_fail("path '${path}' contains a quote or backslash, which the manifest cannot carry")
  endif()
endforeach()

include(CheckCSourceCompiles)
# The probes compile as the archive does, a compiler launcher included.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_C_COMPILER_LAUNCHER)
function(host_reference_probe out_var definitions source)
  set(CMAKE_REQUIRED_INCLUDES "${DSP_ROOT}/Include")
  set(CMAKE_REQUIRED_DEFINITIONS -D__GNUC_PYTHON__ ${definitions})
  set(CMAKE_REQUIRED_QUIET ON)
  # A cached answer would survive a change of CMAKE_C_FLAGS between configures.
  unset(${out_var} CACHE)
  check_c_source_compiles("${source}" ${out_var})
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
  unset(${out_var} CACHE)
endfunction()

# The flags select no code path: as given, before any header can hide them, then as Arm's headers derive them.
function(host_reference_path_macros include)
  set(found "")
  foreach(macro IN LISTS HOST_REFERENCE_PATH_MACROS)
    host_reference_probe(no_${macro} "" "${include}
#if defined(${macro})
#error ${macro}
#endif
int main(void) { return 0; }")
    if(NOT no_${macro})
      list(APPEND found ${macro})
    endif()
  endforeach()
  if(found)
    string(REPLACE ";" ", " found "${found}")
    host_reference_fail("the compile flags select a code path (${found}); the reference is the plain C build")
  endif()
endfunction()
host_reference_probe(trivial_compiles "" "int main(void) { return 0; }")
if(NOT trivial_compiles)
  host_reference_fail("the C compiler does not compile an empty program with these flags "
                      "(${CMAKE_C_COMPILER_LAUNCHER} ${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1} ${CMAKE_C_FLAGS})")
endif()
host_reference_path_macros("")
host_reference_probe(arm_math_compiles "" "#include \"arm_math.h\"
int main(void) { return 0; }")
if(NOT arm_math_compiles)
  host_reference_fail("Include/arm_math.h does not compile for the host with these flags (${CMAKE_C_FLAGS})")
endif()
host_reference_path_macros("#include \"arm_math.h\"")

# float16_t: the compiler's __fp16 where Arm's header defines it, else _Float16 (x86-64), else refuse.
host_reference_probe(native_fp16 "" "
#include \"arm_math_types.h\"
#include \"arm_math_types_f16.h\"
#if !defined(ARM_FLOAT16_SUPPORTED)
#error no float16_t
#endif
int main(void) { volatile float16_t h = (float16_t)1.0f; return (int)(float)h - 1; }")
set(float16_definitions "")
if(native_fp16)
  set(float16_type "__fp16")
else()
  set(float16_definitions "float16_t=_Float16" "ARM_FLOAT16_SUPPORTED=")
  set(probe_definitions "")
  foreach(definition IN LISTS float16_definitions)
    list(APPEND probe_definitions "-D${definition}")
  endforeach()
  host_reference_probe(float16_as_Float16 "${probe_definitions}" "
#include \"arm_math_types.h\"
#include \"arm_math_types_f16.h\"
#if !defined(ARM_FLOAT16_SUPPORTED)
#error no float16_t
#endif
int main(void) { volatile float16_t h = (float16_t)1.0f; return (int)(float)h - 1; }")
  if(NOT float16_as_Float16)
    host_reference_fail("the host C compiler (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}) has no IEEE "
                        "float16 type (__fp16 or _Float16); use one that does, so every f16 kernel has a reference")
  endif()
  set(float16_type "_Float16")
endif()

# How float16 arithmetic is evaluated, pinned and then checked below, so every host with the same float16_t
# gives the same goldens. With __fp16, float16_t is storage-only, as on the Cortex-M55: its arithmetic is done
# in float and rounded at each assignment, and the kernels' explicit _Float16 operations round to half each,
# as the M55's FP16 instructions do. With _Float16, float16_t's own arithmetic is done in float and rounded at
# each assignment and cast, as the M55's float16_t is; the kernels' explicit _Float16 products are then
# rounded once with the sum they feed rather than on their own, so those can differ from the target in the
# last bit. -fexcess-precision=standard gives both: a host with FP16 arithmetic has no excess precision.
set(f16_option "-fexcess-precision=standard")
if(float16_type STREQUAL "__fp16")
  set(f16_evaluation "per operation in half")
  set(f16_probe_expected 2048)
else()
  set(f16_evaluation "in float, rounded at each assignment")
  set(f16_probe_expected 2050)
endif()
include(CheckCCompilerFlag)
unset(f16_option_accepted CACHE)
check_c_compiler_flag("${f16_option}" f16_option_accepted)
if(NOT f16_option_accepted)
  host_reference_fail("${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} does not accept ${f16_option}, which pins "
                      "float16 evaluation; use GCC, or Clang 16 or later")
endif()
unset(f16_option_accepted CACHE)
list(APPEND HOST_REFERENCE_OPTIONS "${f16_option}")

# What the flags do, whatever way they reached the compiler: float and double intermediates are rounded to
# their type (no x87 extended precision), and float16 is evaluated as pinned above. 2048 + 1.0004883 is
# 2050 when the product (1 + 2^-10)(1 - 2^-11) is kept in float, and 2048 when it is first rounded to 1.
include(CheckCSourceRuns)
string(JOIN " " CMAKE_REQUIRED_FLAGS ${HOST_REFERENCE_OPTIONS})
set(CMAKE_REQUIRED_INCLUDES "${DSP_ROOT}/Include")
set(CMAKE_REQUIRED_DEFINITIONS -D__GNUC_PYTHON__)
foreach(definition IN LISTS float16_definitions)
  list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${definition}")
endforeach()
set(CMAKE_REQUIRED_QUIET ON)
foreach(probe IN ITEMS float double float16)
  unset(evaluates_${probe} CACHE)
endforeach()
check_c_source_runs("
int main(void) { volatile float x = 1.0f, y = 5.9604644775390625e-08f; float d = (x + y) - x; return d != 0.0f; }"
                    evaluates_float)
check_c_source_runs("
int main(void) { volatile double x = 1.0, y = 1.1102230246251565e-16; double d = (x + y) - x; return d != 0.0; }"
                    evaluates_double)
check_c_source_runs("
#include \"arm_math_types.h\"
#include \"arm_math_types_f16.h\"
int main(void)
{
    volatile float16_t s = (float16_t)2048.0f, a = (float16_t)1.0009765625f, b = (float16_t)0.99951171875f;
    float16_t r = s;
    r += (_Float16)a * (_Float16)b;
    return (float)r != ${f16_probe_expected}.0f;
}" evaluates_float16)
foreach(probe IN ITEMS float double)
  if(NOT evaluates_${probe})
    host_reference_fail("${probe} intermediates are not rounded to ${probe} with these flags "
                        "(${CMAKE_C_COMPILER_ARG1} ${CMAKE_C_FLAGS} ${CMAKE_REQUIRED_FLAGS}); the reference needs IEEE "
                        "${probe} evaluation (on x86-64, SSE rather than x87)")
  endif()
endforeach()
if(NOT evaluates_float16)
  host_reference_fail("float16_t (${float16_type}) is not evaluated ${f16_evaluation} with these flags "
                      "(${CMAKE_C_COMPILER_ARG1} ${CMAKE_C_FLAGS} ${CMAKE_REQUIRED_FLAGS}), so its goldens would "
                      "depend on the host; with __fp16 the host needs FP16 arithmetic (on AArch64, +fp16 in -march)")
endif()
foreach(probe IN ITEMS float double float16)
  unset(evaluates_${probe} CACHE)
endforeach()
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)

set(HOST ON CACHE BOOL "" FORCE)
set(CMSISDSP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DSP_ROOT}/Source" cmsisdsp)
target_compile_options(CMSISDSP PRIVATE ${HOST_REFERENCE_OPTIONS})
target_compile_definitions(CMSISDSP PUBLIC ${float16_definitions})
# Catches path macros added to the target itself, which the configure probes cannot see.
target_sources(CMSISDSP PRIVATE "${CMAKE_CURRENT_LIST_DIR}/host_reference_guard.c")

set(options ${HOST_REFERENCE_OPTIONS})
string(REPLACE ";" "\", \"" options "${options}")
set(includes "$<FILTER:$<REMOVE_DUPLICATES:$<TARGET_PROPERTY:CMSISDSP,INTERFACE_INCLUDE_DIRECTORIES>>,INCLUDE,.+>")
set(defines "$<FILTER:$<REMOVE_DUPLICATES:$<TARGET_PROPERTY:CMSISDSP,INTERFACE_COMPILE_DEFINITIONS>>,INCLUDE,.+>")
file(GENERATE OUTPUT "${HOST_REFERENCE_MANIFEST}" CONTENT "{
  \"schema\": \"${HOST_REFERENCE_SCHEMA}\",
  \"library\": \"cmsis-dsp\",
  \"source_dir\": \"${DSP_ROOT}\",
  \"archive\": \"$<TARGET_FILE:CMSISDSP>\",
  \"include_dirs\": [\"$<JOIN:${includes},\", \">\"],
  \"compile_definitions\": [\"$<JOIN:${defines},\", \">\"],
  \"library_compile_options\": [\"${options}\"],
  \"link_libraries\": [\"m\"],
  \"float16_type\": \"${float16_type}\",
  \"c_compiler\": {
    \"path\": \"${CMAKE_C_COMPILER}\",
    \"id\": \"${CMAKE_C_COMPILER_ID}\",
    \"version\": \"${CMAKE_C_COMPILER_VERSION}\"
  }
}
")
message(STATUS "CMSIS-DSP host reference: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}, "
               "float16_t = ${float16_type} (evaluated ${f16_evaluation}), manifest ${HOST_REFERENCE_MANIFEST}")
