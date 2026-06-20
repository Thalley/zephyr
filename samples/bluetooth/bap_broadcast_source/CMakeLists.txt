# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bap_broadcast_source)

target_sources(app PRIVATE
  src/main.c
)

if(CONFIG_USE_USB_AUDIO_INPUT)
  include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
endif()

if(CONFIG_USE_PIPEWIRE_AUDIO_INPUT)
  if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL Linux)
    find_package(PkgConfig REQUIRED)
    pkg_search_module(PIPEWIRE REQUIRED libpipewire-0.3)
    list(TRANSFORM PIPEWIRE_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE PIPEWIRE_INCLUDE_DIRS_OPTION)
    target_compile_options(native_simulator INTERFACE "${PIPEWIRE_INCLUDE_DIRS_OPTION}"
                           ${PIPEWIRE_CFLAGS_OTHER})
    list(TRANSFORM PIPEWIRE_LIBRARIES PREPEND "-l" OUTPUT_VARIABLE PIPEWIRE_LIBRARIES_OPTION)
    target_link_options(native_simulator INTERFACE "${PIPEWIRE_LIBRARIES_OPTION}")
    target_sources(app PRIVATE src/pipewire.c)
    target_sources(native_simulator INTERFACE src/pipewire_bottom.c)
  else()
    message(FATAL_ERROR "CONFIG_USE_PIPEWIRE_AUDIO_INPUT is only available on Linux")
  endif()
endif()
