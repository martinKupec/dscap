# dscap — headless DreamSourceLab DSCope/DSLogic capture tool.
#
# Builds a minimal static libsigrok4DSL from the pinned DSView submodule
# (third_party/DSView) and links the dscap CLI against it. The submodule tree is
# only read, never modified.
#
#   git submodule update --init           # fetch third_party/DSView at the pin
#   cmake -S . -B build && cmake --build build -j
#   ./build/dscap enumerate
cmake_minimum_required(VERSION 3.16)
project(dscap C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB   REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(DV "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DSView")
set(SR "${DV}/libsigrok4DSL")
set(CM "${DV}/common")

if(NOT EXISTS "${SR}/lib_main.c")
  message(FATAL_ERROR
    "DSView submodule not found at ${DV}. Run: git submodule update --init")
endif()

# Exact source set DSView compiles into libsigrok4DSL, plus the common xlog and
# minizip it depends on. Mirrors the SRCS list in the Makefile.
set(SIGROK_SRCS
  ${SR}/version.c ${SR}/strutil.c ${SR}/std.c
  ${SR}/session_driver.c ${SR}/session.c ${SR}/log.c
  ${SR}/hwdriver.c ${SR}/error.c ${SR}/backend.c
  ${SR}/trigger.c ${SR}/dsdevice.c ${SR}/lib_main.c
  ${SR}/output/output.c ${SR}/output/csv.c
  ${SR}/output/gnuplot.c ${SR}/output/srzip.c ${SR}/output/vcd.c
  ${SR}/input/input.c ${SR}/input/in_binary.c
  ${SR}/input/in_vcd.c ${SR}/input/in_wav.c
  ${SR}/hardware/demo/demo.c
  ${SR}/hardware/DSL/dslogic.c ${SR}/hardware/DSL/dscope.c
  ${SR}/hardware/DSL/command.c ${SR}/hardware/DSL/dsl.c
  ${SR}/hardware/common/usb.c ${SR}/hardware/common/ezusb.c
  ${CM}/log/xlog.c
  ${CM}/minizip/zip.c ${CM}/minizip/unzip.c ${CM}/minizip/ioapi.c)

add_library(sigrok4DSL STATIC ${SIGROK_SRCS})
target_include_directories(sigrok4DSL PUBLIC ${SR} ${CM})
target_compile_definitions(sigrok4DSL PRIVATE _DEFAULT_SOURCE)
# Vendor sources are not warning-clean; don't let that fail our build.
target_compile_options(sigrok4DSL PRIVATE -w)
target_link_libraries(sigrok4DSL PUBLIC
  PkgConfig::GLIB PkgConfig::LIBUSB ZLIB::ZLIB Threads::Threads m)

add_executable(dscap src/dscap.c src/json.c src/cfg.c src/dsl.c)
target_compile_options(dscap PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(dscap PRIVATE sigrok4DSL)

install(TARGETS dscap RUNTIME DESTINATION bin)
