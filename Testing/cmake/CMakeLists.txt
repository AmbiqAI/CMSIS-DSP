cmake_minimum_required(VERSION 3.23)

project(CMSISDSPTests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMSISDSP_TEST_SUITES "BasicTestsF32" CACHE STRING
    "Test suite classes (desc.txt / desc_f16.txt) to build, one executable each")
# No -ffast-math on the host: it lets the compiler reorder float reductions, so results depend on the host
# compiler. Fast-math correctness is checked on the Cortex-M targets, with the flags that ship.
set(CMSISDSP_TEST_OPT "-O3" CACHE STRING
    "Optimization options for CMSIS-DSP and the tests")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TESTING_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
cmake_path(ABSOLUTE_PATH TESTING_DIR NORMALIZE)

set(HOST ON CACHE BOOL "" FORCE)
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
add_library(dsp_test_framework STATIC ${FRAMEWORK_SOURCES})
target_include_directories(dsp_test_framework PUBLIC ${TESTING_DIR}/FrameworkInclude)
target_compile_definitions(dsp_test_framework PUBLIC EMBEDDED NOTIMING)
target_compile_options(dsp_test_framework PRIVATE ${CMSISDSP_TEST_OPT})
target_link_libraries(dsp_test_framework PUBLIC CMSISDSP)

enable_testing()

function(add_dsp_test_suite suite)
    set(test_source ${TESTING_DIR}/Source/Tests/${suite}.cpp)
    if(NOT EXISTS ${test_source})
        message(FATAL_ERROR "No test source for suite ${suite}: ${test_source}")
    endif()
    if(suite MATCHES "F16$")
        set(pickle ${PICKLE_F16} dsp_pickle_f16)
    else()
        set(pickle ${PICKLE_F32} dsp_pickle_f32)
    endif()

    # processTests.py also writes TestDesc.txt into its working directory, so each suite gets its own.
    set(gen ${CMAKE_BINARY_DIR}/gen/${suite})
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

    add_test(NAME ${suite}
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/suite_status.py -- $<TARGET_FILE:${suite}>)
endfunction()

foreach(suite IN LISTS CMSISDSP_TEST_SUITES)
    add_dsp_test_suite(${suite})
endforeach()

add_test(NAME suite_status_selftest
    COMMAND Python3::Interpreter -m unittest -v test_suite_status test_generate_suite
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
