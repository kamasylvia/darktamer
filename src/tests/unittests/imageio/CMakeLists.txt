# CIRAW v9 loader tests. Only built with USE_CIRAW: the loader itself is
# Apple-only and the v9 camera list covers 69 recent bodies, so no fixture
# ships with the repo. Pass a sample explicitly to run the full assertions:
#
#   cmake -DCIRAW_TEST_SAMPLE=/path/to/v9-body.RAW [...]
#
# without one the binary exits 77, which ctest reports as skipped. The
# non-v9 sample defaults to the integration suite's mire1.cr2 (Canon EOS
# 40D, versions 6/7/8 only, verified 2026-11-03), which exists whenever
# the integration submodule is checked out.
set(CIRAW_TEST_SAMPLE "" CACHE FILEPATH
    "raw file from a CIRAW v9-listed camera for test_ciraw (empty: skip)")
set(CIRAW_TEST_NONV9_SAMPLE "${CMAKE_SOURCE_DIR}/src/tests/integration/images/mire1.cr2"
    CACHE FILEPATH "raw file from a camera outside the CIRAW v9 list")

add_cmocka_test(test_ciraw
                SOURCES test_ciraw.c
                LINK_LIBRARIES lib_darktable cmocka::cmocka)
target_include_directories(test_ciraw PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_ciraw PRIVATE
  CIRAW_TEST_SAMPLE="${CIRAW_TEST_SAMPLE}"
  CIRAW_TEST_NONV9_SAMPLE="${CIRAW_TEST_NONV9_SAMPLE}"
  DT_TEST_DATADIR="${CMAKE_BINARY_DIR}/share/darktable"
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/darktable")
set_tests_properties(test_ciraw PROPERTIES SKIP_RETURN_CODE 77)
