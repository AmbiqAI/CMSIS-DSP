cmake_minimum_required(VERSION 3.23)

project(CMSISDSPTests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMSISDSP_TEST_SUITES "BasicTestsF32" CACHE STRING
    "Test suite classes (desc.txt / desc_f16.txt) to build, one executable each")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TESTING_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
cmake_path(ABSOLUTE_PATH TESTING_DIR NORMALIZE)

# Each variant compiles a different set of code paths. The variant sets the Source/ options itself, so a
# build directory always matches its name.
#   Host:        A: loop unrolling (the default)    B: no loop unrolling
#                C: ARM_MATH_MATRIX_CHECK and ARM_MATH_ROUNDING    I: DISABLEFLOAT16
#   Corstone-300 (toolchains/arm-none-eabi-gcc.cmake):
#                F: Cortex-M55, scalar code written for auto-vectorization (AUTOVECTORIZE), no MVE intrinsics
set(CMSISDSP_TEST_VARIANT "A" CACHE STRING "Build variant: A, B, C, I (host) or F (Corstone-300)")
set_property(CACHE CMSISDSP_TEST_VARIANT PROPERTY STRINGS A B C I F)
set(variant_A LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF)
set(variant_B LOOPUNROLL=OFF MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF)
set(variant_C LOOPUNROLL=ON  MATRIXCHECK=ON  ROUNDING=ON  DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF)
set(variant_I LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=ON  AUTOVECTORIZE=OFF)
set(variant_F LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=ON)
set(target_F corstone300)
set(cpu_flags_F -mcpu=cortex-m55 -mfloat-abi=hard)
if(NOT DEFINED variant_${CMSISDSP_TEST_VARIANT})
    message(FATAL_ERROR "Unknown CMSISDSP_TEST_VARIANT '${CMSISDSP_TEST_VARIANT}': use A, B, C, I or F")
endif()
if(DEFINED target_${CMSISDSP_TEST_VARIANT})
    set(test_target ${target_${CMSISDSP_TEST_VARIANT}})
else()
    set(test_target host)
endif()
if(test_target STREQUAL "host" AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Variant ${CMSISDSP_TEST_VARIANT} is a host build; configure it without a toolchain file")
elseif(NOT test_target STREQUAL "host" AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Variant ${CMSISDSP_TEST_VARIANT} runs on ${test_target}: configure it with "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/toolchains/arm-none-eabi-gcc.cmake")
endif()
foreach(setting IN LISTS variant_${CMSISDSP_TEST_VARIANT})
    string(REPLACE "=" ";" setting ${setting})
    list(GET setting 0 name)
    list(GET setting 1 value)
    set(${name} ${value} CACHE BOOL "" FORCE)
endforeach()
message(STATUS "CMSIS-DSP test variant ${CMSISDSP_TEST_VARIANT} (${test_target}): ${variant_${CMSISDSP_TEST_VARIANT}}")

# No -ffast-math on the host: it lets the compiler reorder float reductions, so results depend on the host
# compiler. The Cortex-M targets use the flags that ship.
if(test_target STREQUAL "host")
    set(default_opt "-O3")
else()
    set(default_opt "-O3;-ffast-math")
endif()
set(CMSISDSP_TEST_OPT "${default_opt}" CACHE STRING "Optimization options for CMSIS-DSP and the tests")

if(test_target STREQUAL "corstone300")
    set(CMSIS_CORE "" CACHE PATH "CMSIS_6 CMSIS/Core directory")
    set(CORTEX_DFP "" CACHE PATH "Cortex_DFP directory")
    set(CORSTONE300_FVP "" CACHE FILEPATH "FVP_Corstone_SSE-300_Ethos-U55 executable")
    option(CMSISDSP_TEST_BUILD_ONLY "Build the images without registering FVP tests" OFF)
    set(CMSISDSP_TEST_TIMEOUT 300 CACHE STRING "Seconds one suite may run on the FVP")
    set(device ${CORTEX_DFP}/Device/ARMCM55)
    if(NOT EXISTS ${CMSIS_CORE}/Include/cmsis_compiler.h)
        message(FATAL_ERROR "CMSIS_CORE must name CMSIS_6 CMSIS/Core (no Include/cmsis_compiler.h in '${CMSIS_CORE}')")
    endif()
    if(NOT EXISTS ${device}/Source/startup_ARMCM55.c)
        message(FATAL_ERROR "CORTEX_DFP must name a Cortex_DFP (no Device/ARMCM55 in '${CORTEX_DFP}')")
    endif()
    if(NOT CMSISDSP_TEST_BUILD_ONLY AND NOT EXISTS "${CORSTONE300_FVP}")
        message(FATAL_ERROR "CORSTONE300_FVP must name the FVP_Corstone_SSE-300_Ethos-U55 executable "
            "(or set CMSISDSP_TEST_BUILD_ONLY=ON to build without running)")
    endif()
    add_compile_options(${cpu_flags_${CMSISDSP_TEST_VARIANT}})
    add_link_options(${cpu_flags_${CMSISDSP_TEST_VARIANT}})
    set(CMSISCORE ${CMSIS_CORE})
    set(HOST OFF CACHE BOOL "" FORCE)
else()
    set(HOST ON CACHE BOOL "" FORCE)
endif()
set(HELIUM OFF CACHE BOOL "" FORCE)
set(MVEI OFF CACHE BOOL "" FORCE)
set(MVEF OFF CACHE BOOL "" FORCE)
set(MVEFLOAT16 OFF CACHE BOOL "" FORCE)
set(NEON OFF CACHE BOOL "" FORCE)
set(NEONEXPERIMENTAL OFF CACHE BOOL "" FORCE)
set(CMSISDSP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${TESTING_DIR}/../Source dsp)

target_compile_definitions(CMSISDSP PRIVATE ARM_DSP_CUSTOM_CONFIG)
target_compile_definitions(CMSISDSP PUBLIC ARM_DSP_TESTING)
target_include_directories(CMSISDSP PRIVATE ${TESTING_DIR})
target_compile_options(CMSISDSP PRIVATE ${CMSISDSP_TEST_OPT} -g)

set(PICKLE_F32 ${CMAKE_BINARY_DIR}/Output.pickle)
set(PICKLE_F16 ${CMAKE_BINARY_DIR}/Output_f16.pickle)
add_custom_command(OUTPUT ${PICKLE_F32}
    COMMAND Python3::Interpreter ${TESTING_DIR}/preprocess.py -f ${TESTING_DIR}/desc.txt -o ${PICKLE_F32}
    DEPENDS ${TESTING_DIR}/desc.txt
    WORKING_DIRECTORY ${TESTING_DIR}
    VERBATIM)
add_custom_command(OUTPUT ${PICKLE_F16}
    COMMAND Python3::Interpreter ${TESTING_DIR}/preprocess.py -f ${TESTING_DIR}/desc_f16.txt -o ${PICKLE_F16}
    DEPENDS ${TESTING_DIR}/desc_f16.txt
    WORKING_DIRECTORY ${TESTING_DIR}
    VERBATIM)

set(FRAMEWORK_SOURCES
    ArrayMemory.cpp Calibrate.cpp Error.cpp FPGA.cpp Generators.cpp IORunner.cpp
    Pattern.cpp PatternMgr.cpp Test.cpp Timing.cpp)
list(TRANSFORM FRAMEWORK_SOURCES PREPEND ${TESTING_DIR}/FrameworkSource/)
if(test_target STREQUAL "corstone300")
    add_library(dsp_test_device INTERFACE)
    target_compile_definitions(dsp_test_device INTERFACE ARMCM55)
    target_include_directories(dsp_test_device INTERFACE
        ${CMSIS_CORE}/Include ${device}/Include ${CMAKE_CURRENT_LIST_DIR}/target)
endif()

add_library(dsp_test_framework STATIC ${FRAMEWORK_SOURCES})
target_include_directories(dsp_test_framework PUBLIC ${TESTING_DIR}/FrameworkInclude)
target_compile_definitions(dsp_test_framework PUBLIC EMBEDDED NOTIMING)
target_compile_options(dsp_test_framework PRIVATE ${CMSISDSP_TEST_OPT})
target_link_libraries(dsp_test_framework PUBLIC CMSISDSP $<TARGET_NAME_IF_EXISTS:dsp_test_device>)

if(test_target STREQUAL "corstone300")
    add_library(dsp_test_target OBJECT
        ${device}/Source/startup_ARMCM55.c
        ${device}/Source/system_ARMCM55.c
        ${TESTING_DIR}/Regression/corstone300_console.c
        ${CMAKE_CURRENT_LIST_DIR}/target/corstone300_glue.c)
    target_link_libraries(dsp_test_target PUBLIC dsp_test_device)
    target_link_options(dsp_test_target INTERFACE
        -T${CMAKE_CURRENT_LIST_DIR}/target/corstone300_tests.ld --specs=nosys.specs -Wl,--gc-sections)
    target_link_libraries(dsp_test_target INTERFACE m)
    set(fvp_settings
        mps3_board.visualisation.disable-visualisation=1
        mps3_board.uart0.out_file=-
        mps3_board.uart0.unbuffered_output=1
        mps3_board.uart0.shutdown_on_eot=1
        mps3_board.uart0.shutdown_tag=EXITTHESIM
        mps3_board.telnetterminal0.start_telnet=0
        mps3_board.telnetterminal1.start_telnet=0
        mps3_board.telnetterminal2.start_telnet=0
        mps3_board.telnetterminal5.start_telnet=0)
    list(TRANSFORM fvp_settings PREPEND "-C;")
endif()

enable_testing()

function(add_dsp_test_suite suite)
    set(test_source ${TESTING_DIR}/Source/Tests/${suite}.cpp)
    if(NOT EXISTS ${test_source})
        message(FATAL_ERROR "No test source for suite ${suite}: ${test_source}")
    endif()
    if(suite MATCHES "F16$")
        if(DISABLEFLOAT16)
            message(FATAL_ERROR "Suite ${suite} needs float16, which variant ${CMSISDSP_TEST_VARIANT} disables")
        endif()
        set(pickle ${PICKLE_F16})
    else()
        set(pickle ${PICKLE_F32})
    endif()

    # processTests.py also writes TestDesc.txt into its working directory, so each suite gets its own.
    set(gen ${CMAKE_BINARY_DIR}/gen/${suite})
    file(MAKE_DIRECTORY ${gen}/GeneratedSource ${gen}/GeneratedInclude)
    set(generated
        ${gen}/GeneratedSource/TestDesc.cpp
        ${gen}/GeneratedInclude/TestDesc.h
        ${gen}/GeneratedInclude/TestDrive.h
        ${gen}/GeneratedInclude/Patterns.h)
    add_custom_command(OUTPUT ${generated}
        COMMAND Python3::Interpreter ${TESTING_DIR}/processTests.py -f ${pickle}
                -p ${TESTING_DIR}/Patterns -d ${TESTING_DIR}/Parameters -gen ${gen} -e ${suite}
        DEPENDS ${pickle} ${TESTING_DIR}/processTests.py ${TESTING_DIR}/TestScripts/CodeGen.py
        WORKING_DIRECTORY ${gen}
        COMMENT "Generating sources for ${suite}"
        VERBATIM)

    add_executable(${suite}
        ${TESTING_DIR}/main.cpp
        ${TESTING_DIR}/testmain.cpp
        ${TESTING_DIR}/patterndata.c
        ${gen}/GeneratedSource/TestDesc.cpp
        ${test_source})
    if(suite MATCHES "^MFCC")
        string(REGEX REPLACE "^MFCC" "" kind ${suite})
        if(kind STREQUAL "F16")
            target_sources(${suite} PRIVATE ${TESTING_DIR}/Source/Tests/mfccdata_f16.c)
        else()
            target_sources(${suite} PRIVATE ${TESTING_DIR}/Source/Tests/mfccdata.c)
        endif()
    endif()
    # The generated headers come first, so a stale Testing/GeneratedInclude is never used.
    target_include_directories(${suite} BEFORE PRIVATE ${gen}/GeneratedInclude)
    target_include_directories(${suite} PRIVATE ${TESTING_DIR}/Include/Tests)
    target_compile_options(${suite} PRIVATE ${CMSISDSP_TEST_OPT})
    target_link_libraries(${suite} PRIVATE dsp_test_framework)

    if(test_target STREQUAL "host")
        add_test(NAME ${suite}
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/suite_status.py -- $<TARGET_FILE:${suite}>)
    else()
        set_target_properties(${suite} PROPERTIES SUFFIX .elf)
        target_link_libraries(${suite} PRIVATE dsp_test_target)
        target_link_options(${suite} PRIVATE -Wl,-Map=$<TARGET_FILE:${suite}>.map)
        if(NOT CMSISDSP_TEST_BUILD_ONLY)
            add_test(NAME ${suite}
                COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/suite_status.py
                        --timeout ${CMSISDSP_TEST_TIMEOUT} -- ${CORSTONE300_FVP} ${fvp_settings}
                        -a $<TARGET_FILE:${suite}>)
        endif()
    endif()
endfunction()

foreach(suite IN LISTS CMSISDSP_TEST_SUITES)
    add_dsp_test_suite(${suite})
endforeach()

add_test(NAME suite_status_selftest
    COMMAND Python3::Interpreter -m unittest -v test_suite_status
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
