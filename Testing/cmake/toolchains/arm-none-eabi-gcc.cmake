# Arm GNU toolchain for bare-metal Cortex-M. The -mcpu and float ABI come from CMSISDSP_TEST_VARIANT.
# Set ARM_GCC_ROOT (CMake or environment) to the toolchain directory, or put arm-none-eabi-gcc on PATH.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_GCC_ROOT AND DEFINED ENV{ARM_GCC_ROOT})
    set(ARM_GCC_ROOT $ENV{ARM_GCC_ROOT})
endif()
if(ARM_GCC_ROOT)
    set(prefix ${ARM_GCC_ROOT}/bin/)
endif()
set(CMAKE_C_COMPILER ${prefix}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${prefix}arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${prefix}arm-none-eabi-gcc)

# A bare-metal executable cannot be linked before the target's startup code and linker script are known.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-mthumb")
set(CMAKE_CXX_FLAGS_INIT "-mthumb")
