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
#   MPS2 AN386 on QEMU (toolchains/arm-none-eabi-gcc.cmake):
#                D: Cortex-M4F with the DSP extension, loop unrolling    E: the same without loop unrolling
#   Corstone-300 (toolchains/arm-none-eabi-gcc.cmake):
#                F: Cortex-M55, scalar code written for auto-vectorization (AUTOVECTORIZE), no MVE intrinsics
#                G: Cortex-M55, Helium intrinsics (MVE-F and MVE-I)
#                H: Cortex-M55 without MVE floating point (+nomve.fp): MVE-I intrinsics, scalar float code
set(CMSISDSP_TEST_VARIANT "A" CACHE STRING "Build variant: A, B, C, I (host), D, E (MPS2 AN386) or F, G, H (Corstone-300)")
set_property(CACHE CMSISDSP_TEST_VARIANT PROPERTY STRINGS A B C I D E F G H)
set(variant_A LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_B LOOPUNROLL=OFF MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_C LOOPUNROLL=ON  MATRIXCHECK=ON  ROUNDING=ON  DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_I LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=ON  AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_D LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_E LOOPUNROLL=OFF MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=OFF)
set(variant_F LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=ON  HELIUM=OFF MVEI=OFF)
set(variant_G LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=ON  MVEI=OFF)
set(variant_H LOOPUNROLL=ON  MATRIXCHECK=OFF ROUNDING=OFF DISABLEFLOAT16=OFF AUTOVECTORIZE=OFF HELIUM=OFF MVEI=ON)
foreach(v D E)
    set(target_${v} mps2_an386)
    set(cpu_flags_${v} -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    # GCC has no __fp16 for Cortex-M4, so the float16 kernels compile to nothing.
    set(no_f16_${v} "Cortex-M4 has no __fp16")
endforeach()
foreach(v F G H)
    set(target_${v} corstone300)
endforeach()
set(cpu_flags_F -mcpu=cortex-m55 -mfloat-abi=hard)
set(cpu_flags_G -mcpu=cortex-m55 -mfloat-abi=hard)
set(cpu_flags_H -mcpu=cortex-m55+nomve.fp -mfloat-abi=hard)
if(NOT DEFINED variant_${CMSISDSP_TEST_VARIANT})
    message(FATAL_ERROR "Unknown CMSISDSP_TEST_VARIANT '${CMSISDSP_TEST_VARIANT}': use A, B, C, D, E, F, G, H or I")
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

option(CMSISDSP_TEST_COVERAGE "Instrument the build for coverage; ctest then writes coverage/coverage-<variant>.json" OFF)
if(CMSISDSP_TEST_COVERAGE)
    # Coverage is measured at -O0, whatever CMSISDSP_TEST_OPT says, so each line keeps its own code. The
    # optimized builds of the same variant check the results at the shipped flags.
    set(dsp_test_opt -O0)
    add_link_options(--coverage)
    if(test_target STREQUAL "host")
        add_compile_options(--coverage)
    else()
        # A target has no file system to write .gcda files to. Each object lists its counters in .gcov_info
        # instead of registering them from a constructor, and target/gcov_dump.c prints them at exit. The
        # options are spelled out, since -fno-profile-arcs cannot undo --coverage for the target support.
        add_compile_options(-fprofile-arcs -ftest-coverage -fprofile-info-section)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        set(default_gcov "xcrun llvm-cov gcov")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        string(REGEX MATCH "^[0-9]+" clang_major ${CMAKE_C_COMPILER_VERSION})
        find_program(llvm_cov NAMES llvm-cov-${clang_major} llvm-cov)
        set(default_gcov "${llvm_cov} gcov")
    else()
        # gcc-13 pairs with gcov-13 and x86_64-linux-gnu-gcc-13 with x86_64-linux-gnu-gcov-13; the real path
        # matters, since the compiler is often reached through cc or c++.
        file(REAL_PATH ${CMAKE_C_COMPILER} compiler_path)
        get_filename_component(compiler_dir ${compiler_path} DIRECTORY)
        get_filename_component(compiler_name ${compiler_path} NAME)
        string(REGEX MATCH "^[0-9]+" gcc_major ${CMAKE_C_COMPILER_VERSION})
        if(compiler_name MATCHES "gcc")
            string(REPLACE "gcc" "gcov" gcov_name ${compiler_name})
            set(default_gcov ${compiler_dir}/${gcov_name})
        else()
            find_program(gcov_by_version NAMES gcov-${gcc_major} gcov NO_CACHE)
            set(default_gcov ${gcov_by_version})
        endif()
    endif()
    set(CMSISDSP_TEST_GCOV "${default_gcov}" CACHE STRING "gcov command that reads this compiler's coverage data")
    separate_arguments(gcov_command NATIVE_COMMAND "${CMSISDSP_TEST_GCOV}")
    execute_process(COMMAND ${gcov_command} --version
        RESULT_VARIABLE gcov_status OUTPUT_VARIABLE gcov_version ERROR_QUIET)
    if(NOT gcov_status EQUAL 0 OR NOT gcov_version MATCHES "gcov|LLVM")
        message(FATAL_ERROR "CMSISDSP_TEST_GCOV '${CMSISDSP_TEST_GCOV}' is not a working gcov")
    endif()
    # A gcov from another GCC release cannot read this compiler's data files: their version names the major
    # and the minor release.
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" gcc_release "${CMAKE_C_COMPILER_VERSION}")
    string(REPLACE "." "\\." gcc_release_regex "${gcc_release}")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT gcov_version MATCHES "gcov[^\n]* ${gcc_release_regex}\\.")
        message(FATAL_ERROR "CMSISDSP_TEST_GCOV '${CMSISDSP_TEST_GCOV}' does not match GCC ${CMAKE_C_COMPILER_VERSION}")
    endif()
    if(NOT test_target STREQUAL "host")
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "Coverage on ${test_target} needs GCC (-fprofile-info-section), not "
                "${CMAKE_C_COMPILER_ID}")
        endif()
        # decode_gcov.py turns the console streams into .gcda files with this compiler's gcov-tool.
        string(REPLACE "gcc" "gcov-tool" gcov_tool_name ${compiler_name})
        set(CMSISDSP_TEST_GCOV_TOOL ${compiler_dir}/${gcov_tool_name} CACHE FILEPATH
            "gcov-tool that merges the targets' coverage streams")
        execute_process(COMMAND ${CMSISDSP_TEST_GCOV_TOOL} --version
            RESULT_VARIABLE gcov_tool_status OUTPUT_VARIABLE gcov_tool_version ERROR_QUIET)
        if(NOT gcov_tool_status EQUAL 0 OR NOT gcov_tool_version MATCHES "gcov-tool[^\n]* ${gcc_release_regex}\\.")
            message(FATAL_ERROR "CMSISDSP_TEST_GCOV_TOOL '${CMSISDSP_TEST_GCOV_TOOL}' is not the gcov-tool of "
                "GCC ${CMAKE_C_COMPILER_VERSION}")
        endif()
    endif()
    if(Python3_VERSION VERSION_LESS 3.11)
        message(FATAL_ERROR "Coverage builds need Python 3.11 or later (the gate reads TOML with tomllib); "
            "${Python3_EXECUTABLE} is ${Python3_VERSION}")
    endif()
    execute_process(COMMAND ${Python3_EXECUTABLE} -m gcovr --version
        RESULT_VARIABLE gcovr_status OUTPUT_QUIET ERROR_QUIET)
    if(NOT gcovr_status EQUAL 0)
        message(FATAL_ERROR "gcovr is not installed for ${Python3_EXECUTABLE}: "
            "pip install -r ${TESTING_DIR}/requirements-ci.txt")
    endif()
else()
    set(dsp_test_opt ${CMSISDSP_TEST_OPT})
endif()

if(NOT test_target STREQUAL "host")
    set(CMSIS_CORE "" CACHE PATH "CMSIS_6 CMSIS/Core directory")
    set(CORTEX_DFP "" CACHE PATH "Cortex_DFP directory")
    option(CMSISDSP_TEST_BUILD_ONLY "Build the images without registering tests that run them" OFF)
    set(CMSISDSP_TEST_TIMEOUT 300 CACHE STRING "Seconds one suite may run on the model")
    set(device_corstone300 ARMCM55)
    set(device_mps2_an386 ARMCM4)
    set(device_name ${device_${test_target}})
    set(device ${CORTEX_DFP}/Device/${device_name})
    if(NOT EXISTS ${CMSIS_CORE}/Include/cmsis_compiler.h)
        message(FATAL_ERROR "CMSIS_CORE must name CMSIS_6 CMSIS/Core (no Include/cmsis_compiler.h in '${CMSIS_CORE}')")
    endif()
    foreach(dfp_file Source/startup_${device_name}.c Source/system_${device_name}.c Include/${device_name}.h)
        if(NOT EXISTS ${device}/${dfp_file})
            message(FATAL_ERROR "CORTEX_DFP must name a Cortex_DFP (no Device/${device_name}/${dfp_file} in '${CORTEX_DFP}')")
        endif()
    endforeach()
    if(test_target STREQUAL "corstone300")
        set(CORSTONE300_FVP "" CACHE FILEPATH "FVP_Corstone_SSE-300_Ethos-U55 executable")
        set(model "${CORSTONE300_FVP}")
        set(model_hint "CORSTONE300_FVP must name the FVP_Corstone_SSE-300_Ethos-U55 executable")
    else()
        find_program(QEMU_SYSTEM_ARM qemu-system-arm)
        set(model "${QEMU_SYSTEM_ARM}")
        set(model_hint "QEMU_SYSTEM_ARM must name the qemu-system-arm executable (found '${QEMU_SYSTEM_ARM}')")
    endif()
    if(NOT CMSISDSP_TEST_BUILD_ONLY AND NOT EXISTS "${model}")
        message(FATAL_ERROR "${model_hint} (or set CMSISDSP_TEST_BUILD_ONLY=ON to build without running)")
    endif()
    add_compile_options(${cpu_flags_${CMSISDSP_TEST_VARIANT}})
    add_link_options(${cpu_flags_${CMSISDSP_TEST_VARIANT}})
    set(CMSISCORE ${CMSIS_CORE})
    set(HOST OFF CACHE BOOL "" FORCE)
else()
    set(HOST ON CACHE BOOL "" FORCE)
endif()
set(MVEF OFF CACHE BOOL "" FORCE)
set(MVEFLOAT16 OFF CACHE BOOL "" FORCE)
set(NEON OFF CACHE BOOL "" FORCE)
set(NEONEXPERIMENTAL OFF CACHE BOOL "" FORCE)
set(CMSISDSP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${TESTING_DIR}/../Source dsp)
if(MVEI AND NOT HELIUM)
    # Source/CommonTables/Config.cmake adds the MVE tables only for HELIUM or MVEF; the MVE-I kernels need them too.
    target_sources(CMSISDSP PRIVATE ${TESTING_DIR}/../Source/CommonTables/arm_mve_tables.c)
endif()

target_compile_definitions(CMSISDSP PRIVATE ARM_DSP_CUSTOM_CONFIG)
target_compile_definitions(CMSISDSP PUBLIC ARM_DSP_TESTING)
target_include_directories(CMSISDSP PRIVATE ${TESTING_DIR})
target_compile_options(CMSISDSP PRIVATE ${dsp_test_opt} -g)

set(PICKLE_F32 ${CMAKE_BINARY_DIR}/Output.pickle)
set(PICKLE_F16 ${CMAKE_BINARY_DIR}/Output_f16.pickle)
add_custom_command(OUTPUT ${PICKLE_F32}
    COMMAND Python3::Interpreter ${TESTING_DIR}/preprocess.py -f ${TESTING_DIR}/desc.txt -o ${PICKLE_F32}
    DEPENDS ${TESTING_DIR}/desc.txt
    WORKING_DIRECTORY ${TESTING_DIR}
    COMMENT "Parsing desc.txt"
    VERBATIM)
add_custom_command(OUTPUT ${PICKLE_F16}
    COMMAND Python3::Interpreter ${TESTING_DIR}/preprocess.py -f ${TESTING_DIR}/desc_f16.txt -o ${PICKLE_F16}
    DEPENDS ${TESTING_DIR}/desc_f16.txt
    WORKING_DIRECTORY ${TESTING_DIR}
    COMMENT "Parsing desc_f16.txt"
    VERBATIM)
# Each pickle is built by its own target, so suites that share it never run the parser concurrently.
add_custom_target(dsp_pickle_f32 DEPENDS ${PICKLE_F32})
add_custom_target(dsp_pickle_f16 DEPENDS ${PICKLE_F16})

set(FRAMEWORK_SOURCES
    ArrayMemory.cpp Calibrate.cpp Error.cpp FPGA.cpp Generators.cpp IORunner.cpp
    Pattern.cpp PatternMgr.cpp Test.cpp Timing.cpp)
list(TRANSFORM FRAMEWORK_SOURCES PREPEND ${TESTING_DIR}/FrameworkSource/)
if(NOT test_target STREQUAL "host")
    add_library(dsp_test_device INTERFACE)
    target_compile_definitions(dsp_test_device INTERFACE ${device_name} CMSIS_device_header="${device_name}.h")
    target_include_directories(dsp_test_device INTERFACE
        ${CMSIS_CORE}/Include ${device}/Include ${CMAKE_CURRENT_LIST_DIR}/target)
endif()

add_library(dsp_test_framework STATIC ${FRAMEWORK_SOURCES})
target_include_directories(dsp_test_framework PUBLIC ${TESTING_DIR}/FrameworkInclude)
target_compile_definitions(dsp_test_framework PUBLIC EMBEDDED NOTIMING)
# -ffast-math lets GCC drop the isnan/isinf guards in Error.cpp, and a NaN SNR then passes `snr < threshold`.
# The framework's checks build without it; the kernels and the suites keep the shipped flags.
target_compile_options(dsp_test_framework PRIVATE ${dsp_test_opt} -fno-fast-math)
target_link_libraries(dsp_test_framework PUBLIC CMSISDSP $<TARGET_NAME_IF_EXISTS:dsp_test_device>)

if(NOT test_target STREQUAL "host")
    add_library(dsp_test_target OBJECT
        ${device}/Source/startup_${device_name}.c
        ${device}/Source/system_${device_name}.c
        ${CMAKE_CURRENT_LIST_DIR}/target/runtime.c
        ${CMAKE_CURRENT_LIST_DIR}/target/${test_target}_glue.c)
    target_link_libraries(dsp_test_target PUBLIC dsp_test_device)
    target_link_options(dsp_test_target INTERFACE
        -L${CMAKE_CURRENT_LIST_DIR}/target -T${CMAKE_CURRENT_LIST_DIR}/target/${test_target}_tests.ld
        --specs=nosys.specs -Wl,--gc-sections)
    # -T and the script's INCLUDE are not dependencies CMake sees; without this an edited layout reuses old images.
    set_property(TARGET dsp_test_target APPEND PROPERTY INTERFACE_LINK_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/target/${test_target}_tests.ld ${CMAKE_CURRENT_LIST_DIR}/target/tests_sections.ld)
    target_link_libraries(dsp_test_target INTERFACE m)
    if(CMSISDSP_TEST_COVERAGE)
        target_sources(dsp_test_target PRIVATE ${CMAKE_CURRENT_LIST_DIR}/target/gcov_dump.c)
        # Start-up code runs before the counters are set up and the export runs while they are written, so
        # none of the target support is measured.
        target_compile_options(dsp_test_target PRIVATE -fno-profile-arcs -fno-test-coverage)
    endif()
endif()
if(test_target STREQUAL "corstone300")
    target_sources(dsp_test_target PRIVATE ${TESTING_DIR}/Regression/corstone300_console.c)
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
    set(run_image ${CORSTONE300_FVP} ${fvp_settings} -a)
elseif(test_target STREQUAL "mps2_an386")
    set(run_image ${QEMU_SYSTEM_ARM} -M mps2-an386 -cpu cortex-m4 -display none -monitor none -serial stdio
        -semihosting-config enable=on,target=native -kernel)
endif()

enable_testing()

# Generates the framework sources for one suite class, once, however many images use them.
function(generate_suite_sources suite gen_var)
    set(gen ${CMAKE_BINARY_DIR}/gen/${suite})
    set(${gen_var} ${gen} PARENT_SCOPE)
    get_property(done GLOBAL PROPERTY dsp_generated_${suite})
    if(done)
        return()
    endif()
    set_property(GLOBAL PROPERTY dsp_generated_${suite} TRUE)
    if(suite MATCHES "F16$")
        if(DISABLEFLOAT16)
            message(FATAL_ERROR "Suite ${suite} needs float16, which variant ${CMSISDSP_TEST_VARIANT} disables")
        elseif(DEFINED no_f16_${CMSISDSP_TEST_VARIANT})
            message(FATAL_ERROR "Suite ${suite} needs float16: ${no_f16_${CMSISDSP_TEST_VARIANT}}")
        endif()
        set(pickle ${PICKLE_F16} dsp_pickle_f16)
    else()
        set(pickle ${PICKLE_F32} dsp_pickle_f32)
    endif()

    # processTests.py also writes TestDesc.txt into its working directory, so each suite gets its own.
    file(MAKE_DIRECTORY ${gen}/GeneratedSource ${gen}/GeneratedInclude)
    set(generated
        ${gen}/GeneratedSource/TestDesc.cpp
        ${gen}/GeneratedInclude/TestDesc.h
        ${gen}/GeneratedInclude/TestDrive.h
        ${gen}/GeneratedInclude/Patterns.h)
    # The depfile lists the pattern and parameter files the suite embeds, so editing one regenerates it.
    list(GET pickle 0 pickle_file)
    list(GET generated 0 first_output)
    add_custom_command(OUTPUT ${generated}
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/generate_suite.py
                --depfile ${gen}/sources.d --output ${first_output}
                -- ${TESTING_DIR}/processTests.py -f ${pickle_file}
                -p ${TESTING_DIR}/Patterns -d ${TESTING_DIR}/Parameters -gen ${gen} -e ${suite}
        DEPENDS ${pickle} ${TESTING_DIR}/processTests.py ${TESTING_DIR}/TestScripts/CodeGen.py
                ${CMAKE_CURRENT_LIST_DIR}/generate_suite.py
        DEPFILE ${gen}/sources.d
        WORKING_DIRECTORY ${gen}
        COMMENT "Generating sources for ${suite}"
        VERBATIM)
    # The images of one suite share these sources, so one target owns the rule and the images depend on it.
    add_custom_target(dsp_gen_${suite} DEPENDS ${generated})
endfunction()

# add_dsp_test_suite(<suite> [NAME <test>] [EXPECT_FAILURES] [SOURCES <extra sources>...])
# Builds one image of a suite class and registers a test that runs it. EXPECT_FAILURES makes the test a
# negative control (suite_status.py --expect-failures).
function(add_dsp_test_suite suite)
    cmake_parse_arguments(PARSE_ARGV 1 arg "EXPECT_FAILURES" "NAME" "SOURCES")
    set(name ${suite})
    if(arg_NAME)
        set(name ${arg_NAME})
    endif()
    set(test_source ${TESTING_DIR}/Source/Tests/${suite}.cpp)
    if(NOT EXISTS ${test_source})
        message(FATAL_ERROR "No test source for suite ${suite}: ${test_source}")
    endif()
    generate_suite_sources(${suite} gen)

    add_executable(${name}
        ${arg_SOURCES}
        ${TESTING_DIR}/main.cpp
        ${TESTING_DIR}/testmain.cpp
        ${TESTING_DIR}/patterndata.c
        ${gen}/GeneratedSource/TestDesc.cpp
        ${test_source})
    if(suite MATCHES "^MFCC")
        string(REGEX REPLACE "^MFCC" "" kind ${suite})
        if(kind STREQUAL "F16")
            target_sources(${name} PRIVATE ${TESTING_DIR}/Source/Tests/mfccdata_f16.c)
        else()
            target_sources(${name} PRIVATE ${TESTING_DIR}/Source/Tests/mfccdata.c)
        endif()
    endif()
    # The generated headers come first, so a stale Testing/GeneratedInclude is never used.
    target_include_directories(${name} BEFORE PRIVATE ${gen}/GeneratedInclude)
    target_include_directories(${name} PRIVATE ${TESTING_DIR}/Include/Tests)
    target_compile_options(${name} PRIVATE ${dsp_test_opt})
    target_link_libraries(${name} PRIVATE dsp_test_framework)
    add_dependencies(${name} dsp_gen_${suite})

    set(status_args)
    if(arg_EXPECT_FAILURES)
        list(APPEND status_args --expect-failures)
    endif()
    if(test_target STREQUAL "host")
        add_test(NAME ${name}
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/suite_status.py ${status_args}
                    -- $<TARGET_FILE:${name}>)
        if(CMSISDSP_TEST_COVERAGE)
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED dsp_coverage)
        endif()
    else()
        set_target_properties(${name} PROPERTIES SUFFIX .elf)
        target_link_libraries(${name} PRIVATE dsp_test_target)
        target_link_options(${name} PRIVATE -Wl,-Map=$<TARGET_FILE:${name}>.map)
        if(NOT CMSISDSP_TEST_BUILD_ONLY)
            if(CMSISDSP_TEST_COVERAGE)
                # The console carries the image's counters; decode_gcov.py reads them from this transcript.
                set(transcript ${CMAKE_BINARY_DIR}/coverage/console/${name}.txt)
                list(APPEND status_args --transcript ${transcript})
                set_property(GLOBAL APPEND PROPERTY dsp_coverage_transcripts ${transcript})
            endif()
            add_test(NAME ${name}
                COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/suite_status.py ${status_args}
                        --timeout ${CMSISDSP_TEST_TIMEOUT} -- ${run_image} $<TARGET_FILE:${name}>)
            if(CMSISDSP_TEST_COVERAGE)
                set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED dsp_coverage)
            endif()
        endif()
    endif()
endfunction()

foreach(suite IN LISTS CMSISDSP_TEST_SUITES)
    add_dsp_test_suite(${suite})
endforeach()

# Every build also runs a control that must fail, so a model, console or parser that loses failures is
# caught on the target where it happens. A coverage build leaves it out: it shares the library's counters,
# so its runs would count as coverage. The optimized build of the same variant runs it.
if(NOT CMSISDSP_TEST_COVERAGE)
    add_dsp_test_suite(BasicTestsF32 NAME planted_failure_control EXPECT_FAILURES
        SOURCES ${CMAKE_CURRENT_LIST_DIR}/planted_failure.c)
    # The suite's calls are renamed at compile time, not overridden at link time: with FASTBUILD the library's
    # arm_add_f32 shares an archive member with functions the suite needs, and the two definitions collide.
    target_compile_definitions(planted_failure_control PRIVATE arm_add_f32=planted_arm_add_f32)
elseif(test_target STREQUAL "host" OR NOT CMSISDSP_TEST_BUILD_ONLY)
    # Counters are cleared before the suites run and exported after the last one, so a report only ever
    # holds the current run.
    set(transcript_dir ${CMAKE_BINARY_DIR}/coverage/console)
    add_test(NAME coverage_reset
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/host_coverage.py reset --build-dir ${CMAKE_BINARY_DIR}
                --transcripts ${transcript_dir})
    add_test(NAME coverage_report
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/host_coverage.py report
                --build-dir ${CMAKE_BINARY_DIR} --root ${TESTING_DIR}/.. --gcov ${CMSISDSP_TEST_GCOV}
                --output ${CMAKE_BINARY_DIR}/coverage/coverage-${CMSISDSP_TEST_VARIANT}.json
                --summary ${CMAKE_BINARY_DIR}/coverage/summary-${CMSISDSP_TEST_VARIANT}.json)
    set_tests_properties(coverage_reset PROPERTIES FIXTURES_SETUP dsp_coverage)
    set_tests_properties(coverage_report PROPERTIES FIXTURES_CLEANUP dsp_coverage)
    if(NOT test_target STREQUAL "host")
        # Every suite's transcript must hold a complete stream. A failed decode merges nothing, and the report
        # that follows then fails for want of counters.
        get_property(transcripts GLOBAL PROPERTY dsp_coverage_transcripts)
        add_test(NAME coverage_decode
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/decode_gcov.py
                    --gcov-tool ${CMSISDSP_TEST_GCOV_TOOL} --build-dir ${CMAKE_BINARY_DIR} ${transcripts})
        set_tests_properties(coverage_decode PROPERTIES FIXTURES_CLEANUP dsp_coverage)
        set_tests_properties(coverage_report PROPERTIES DEPENDS coverage_decode)
    endif()
endif()
if(CMSISDSP_TEST_COVERAGE)
    # They need gcovr and Python 3.11, which configure checks only in coverage builds.
    add_test(NAME coverage_merge_selftest
        COMMAND Python3::Interpreter -m unittest -v test_merge
        WORKING_DIRECTORY ${TESTING_DIR}/coverage)
    add_test(NAME coverage_gate_selftest
        COMMAND Python3::Interpreter -m unittest -v test_coverage_gate
        WORKING_DIRECTORY ${TESTING_DIR}/coverage)
endif()

# The NaN control checks the framework at the build's flags. A coverage build (-O0, no fast-math) leaves it
# out, like the planted-failure control, so its runs never count as coverage.
if(NOT CMSISDSP_TEST_COVERAGE)
    add_executable(framework_nan_control ${CMAKE_CURRENT_LIST_DIR}/framework_nan_control.cpp)
    target_compile_options(framework_nan_control PRIVATE ${dsp_test_opt})
    target_link_libraries(framework_nan_control PRIVATE dsp_test_framework)
    if(test_target STREQUAL "host")
        add_test(NAME framework_nan_control COMMAND framework_nan_control)
    else()
        set_target_properties(framework_nan_control PROPERTIES SUFFIX .elf)
        target_link_libraries(framework_nan_control PRIVATE dsp_test_target)
        if(NOT CMSISDSP_TEST_BUILD_ONLY)
            add_test(NAME framework_nan_control COMMAND ${run_image} $<TARGET_FILE:framework_nan_control>)
            set_tests_properties(framework_nan_control PROPERTIES TIMEOUT ${CMSISDSP_TEST_TIMEOUT})
        endif()
    endif()
    if(TEST framework_nan_control)
        # The FVP exits 0 whatever the program returns, so the verdict comes from the output on every target.
        set_tests_properties(framework_nan_control PROPERTIES
            PASS_REGULAR_EXPRESSION "NAN-CONTROL PASS"
            FAIL_REGULAR_EXPRESSION "NAN-CONTROL FAIL;E: HardFault")
    endif()
endif()

if(NOT test_target STREQUAL "host")
    # A fault before main(), before testmain_hook() starts the console, must still report and end the run.
    add_executable(startup_fault_control ${CMAKE_CURRENT_LIST_DIR}/startup_fault_control.c)
    set_target_properties(startup_fault_control PROPERTIES SUFFIX .elf)
    target_link_libraries(startup_fault_control PRIVATE dsp_test_target)
    if(NOT CMSISDSP_TEST_BUILD_ONLY)
        add_test(NAME startup_fault_control COMMAND ${run_image} $<TARGET_FILE:startup_fault_control>)
        set_tests_properties(startup_fault_control PROPERTIES
            TIMEOUT ${CMSISDSP_TEST_TIMEOUT}
            PASS_REGULAR_EXPRESSION "E: HardFault"
            FAIL_REGULAR_EXPRESSION "STARTUP-FAULT-CONTROL REACHED MAIN")
    endif()
endif()

add_test(NAME suite_status_selftest
    COMMAND Python3::Interpreter -m unittest -v test_suite_status test_generate_suite test_host_coverage
            test_decode_gcov
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})

# The inventory and the CI plan read the sources as text, so their tests need no coverage build.
add_test(NAME coverage_inventory_selftest
    COMMAND Python3::Interpreter -m unittest -v test_inventory test_ci_plan
    WORKING_DIRECTORY ${TESTING_DIR}/coverage)
