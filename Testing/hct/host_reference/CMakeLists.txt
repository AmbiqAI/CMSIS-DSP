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
set(HOST_REFERENCE_UNSAFE_FP
    "(^|[ ;])(-Ofast|-ffast-math|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math|-ffinite-math-only|-ffp-contract=(fast|on))([ ;]|$)")
# Source/ options that change what the reference computes or builds; each must be OFF. ROUNDING and
# MATRIXCHECK change results and status codes, and the manifest cannot say they were set.
set(HOST_REFERENCE_OFF_OPTIONS
    HELIUM HELIUMEXPERIMENTAL MVEF MVEI MVEFLOAT16 NEON NEONEXPERIMENTAL AUTOVECTORIZE LOOPUNROLL DISABLEFLOAT16
    FASTBUILD ROUNDING MATRIXCHECK)

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
foreach(name IN LISTS HOST_REFERENCE_OFF_OPTIONS)
  if(DEFINED ${name} AND ${name})
    host_reference_fail("${name}=${${name}} changes what the reference computes or builds; leave it OFF")
  endif()
  set(${name} OFF CACHE BOOL "" FORCE)
endforeach()

set(flag_vars CMAKE_C_FLAGS)
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
               "float16_t = ${float16_type}, manifest ${HOST_REFERENCE_MANIFEST}")
