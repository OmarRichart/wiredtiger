#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

project(workgen CXX)

# Skip compiling workgen on non-Posix systems. Currently workgen isn't supported
# for our Windows builds.
if (NOT WT_POSIX)
    return()
endif()

# UseSWIG: Policy option to use the standard target name over a generated name.
if(POLICY CMP0078)
    cmake_policy(SET CMP0078 OLD)
endif()

# UseSWIG: Policy option to honor SWIG_MODULE_NAME.
if(POLICY CMP0086)
    cmake_policy(SET CMP0086 NEW)
endif()

find_package(SWIG 3.0 COMPONENTS python REQUIRED)
include(${SWIG_USE_FILE})
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

# Determine which wiredtiger target we should use to link against the
# Python Workgen API. For linking purposes, we need to ensure the wiredtiger
# library was compiled as position independent code (fPIC). This being achieved
# either using the 'WITH_PIC' configuration option or building a shared library
# version of WiredTiger ('ENABLE_SHARED'). Default to whichever option is available
# (with a preference for a PIC static build), otherwise throw an error if neither
# build is available.
set(wiredtiger_target)
if(ENABLE_STATIC AND WITH_PIC)
    set(wiredtiger_target wiredtiger_static)
elseif(ENABLE_SHARED)
    set(wiredtiger_target wiredtiger_shared)
else()
    message(STATIC "Skipping workgen compiliation: Requires either a shared library (ENABLE_SHARED)"
                    "or a PIC-enabled (WITH_PIC) build of wiredtiger")
endif()

set(python_libs)
set(python_version)
set(python_executable)
# Source the user's Python3 install, extracting the Python 3 version, library and interpreter install paths.
source_python3_package(python_libs python_version python_executable)

# Compile the intermediate C++ workgen sources into static library.
add_library(
    workgen_cxx
    STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/workgen.cxx
        ${CMAKE_CURRENT_SOURCE_DIR}/workgen_func.c
)
target_include_directories(
    workgen_cxx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_BINARY_DIR}/config
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/test/utility
)
set_property(TARGET workgen_cxx PROPERTY POSITION_INDEPENDENT_CODE ON)

# Manually specify the interface name '_workgen' over a generated name.
# Our workgen tooling expects the swig interface name '_workgen'
# when importing the generated swig library.
list(APPEND swig_flags "-c++")
list(APPEND swig_flags "-interface;_workgen")
list(APPEND swig_flags "-threads")
list(APPEND swig_flags "-O")
list(APPEND swig_flags "-Wall")
# Pass in our generated include directories.
list(APPEND swig_flags "-I${CMAKE_BINARY_DIR}/include")
list(APPEND swig_flags "-I${CMAKE_BINARY_DIR}/config")

set(CMAKE_SWIG_FLAGS ${swig_flags})
# Indicate the swig file should be compiled in C++ mode.
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/workgen.i PROPERTY CPLUSPLUS ON)
swig_add_library(_workgen
  TYPE SHARED
  LANGUAGE python
  SOURCES ${CMAKE_CURRENT_LIST_DIR}/workgen.i
)
# Capture the target name generated by 'swig_add_library'. On older versions of cmake (< 3.13),
# CMP0078 policy is default set to OLD (leading to a potentially different target name).
set(swig_workgen_target ${SWIG_MODULE__workgen_REAL_NAME})

swig_link_libraries(_workgen workgen_cxx ${wiredtiger_target} ${python_libs})
# Force the output name of the shared library to '_workgen'. Various tooling in
# WiredTiger expects to find a binary named '_workgen.so'.
set_target_properties(${swig_workgen_target} PROPERTIES OUTPUT_NAME "_workgen")

# Depending on the chosen compiler (cl, clang or gcc), a slightly different set of compiler flags
# will be used.
set(swig_cxx_flags)
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(swig_cxx_flags
        # Ignore sign conversion warning.
        /wd4244
        /w
    )
else()
    set(swig_cxx_flags
        -Wno-sign-conversion
        -w
        -std=c++11
    )
endif()
list(APPEND swig_cxx_flags -I${CMAKE_SOURCE_DIR})
list(APPEND swig_cxx_flags -I${CMAKE_BINARY_DIR}/config)
list(APPEND swig_cxx_flags -I${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(${swig_workgen_target}
    PRIVATE ${swig_cxx_flags}
)

# Manually changing the suffix on macOS/Darwin builds since DYLD_LIBRARY_PATH
# doesn't work well for dynamic modules loaded by python.
if(WT_DARWIN)
    set_target_properties(${swig_workgen_target} PROPERTIES SUFFIX ".so")
endif()
