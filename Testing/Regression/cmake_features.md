# Header-affecting CMake feature options

Run `python3 Testing/Regression/test_cmake_features.py` from any directory.
The test uses CMake, CTest, and the host C compiler. It creates temporary build
and installation directories and downloads nothing.

A small producer is configured using the actual `Source/configDsp.cmake` helper.
The consumer receives usage requirements only through `target_link_libraries`.
Both report the configuration they compiled with, and the executable requires
them to match each other and the requested options. All eight combinations of
`AUTOVECTORIZE`, `MVEFLOAT16`, and `DISABLEFLOAT16` are exercised for both in-tree
and installed/exported target consumption (16 executions).

This isolates transitive CMake configuration behavior; it is not a numerical
kernel test or a complete CMSIS-DSP package installation test.

For a negative control, save the upstream `configDsp.cmake` outside the checkout
and pass `--config-dsp-file /path/to/configDsp.cmake`. Upstream fails the first
nonzero feature mask: `library=1 consumer=0 expected=1`.
