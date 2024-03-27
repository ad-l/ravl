
set(BUILD_QCBOR_TEST  "OFF"  CACHE STRING "Build QCBOR test suite [OFF, LIB, APP]")
set(BUILD_QCBOR_WARN   OFF   CACHE BOOL   "Compile with the warning flags used in the QCBOR release process")
# BUILD_SHARED_LIBS is a built-in global CMake flag
#    The shared library is not made by default because of platform
#    variability For example MacOS and Linux behave differently and some
#    IoT OS's don't support them at all.
set(BUILD_SHARED_LIBS  OFF   CACHE BOOL   "Build shared libraries instead of static ones")

# Configuration:
#   Floating-point support (see README.md for more information)
set(QCBOR_OPT_DISABLE_FLOAT_HW_USE     OFF  CACHE BOOL "Eliminate dependency on FP hardware and FP instructions")
set(QCBOR_OPT_DISABLE_FLOAT_PREFERRED  OFF  CACHE BOOL "Eliminate support for half-precision and CBOR preferred serialization")
set(QCBOR_OPT_DISABLE_FLOAT_ALL        OFF  CACHE BOOL "Eliminate floating-point support completely")

if (BUILD_QCBOR_WARN)
    # Compile options applying to all targets in current directory and below
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wcast-qual)
endif()

add_library(qcbor)

set(QCBOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/QCBOR")
set(QCBOR_SRC "${QCBOR_DIR}/src")
target_sources(qcbor
    PRIVATE
        ${QCBOR_SRC}/ieee754.c
        ${QCBOR_SRC}/qcbor_decode.c
        ${QCBOR_SRC}/qcbor_encode.c
        ${QCBOR_SRC}/qcbor_err_to_str.c
        ${QCBOR_SRC}/UsefulBuf.c
)

target_include_directories(qcbor
    PUBLIC
        $<BUILD_INTERFACE:${QCBOR_DIR}/inc>
        $<INSTALL_INTERFACE:inc>
    PRIVATE
        src
)

target_compile_definitions(qcbor
    PRIVATE
        $<$<BOOL:${QCBOR_OPT_DISABLE_FLOAT_HW_USE}>:QCBOR_DISABLE_FLOAT_HW_USE>
        $<$<BOOL:${QCBOR_OPT_DISABLE_FLOAT_PREFERRED}>:QCBOR_DISABLE_PREFERRED_FLOAT>
        $<$<BOOL:${QCBOR_OPT_DISABLE_FLOAT_ALL}>:USEFULBUF_DISABLE_ALL_FLOAT>
)

if (BUILD_SHARED_LIBS)
    target_compile_options(qcbor PRIVATE -Os -fPIC)
endif()

# The math library is needed for floating-point support.
# To avoid need for it #define QCBOR_DISABLE_FLOAT_HW_USE
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # Using GCC
    target_link_libraries(qcbor
        PRIVATE
            $<$<NOT:$<BOOL:${QCBOR_OPT_DISABLE_FLOAT_HW_USE}>>:m>
    )
endif()

if (NOT BUILD_QCBOR_TEST STREQUAL "OFF")
    add_subdirectory(${QCBOR_DIR}/test)
endif()
