# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bap_broadcast_sink)

target_sources(app PRIVATE
  src/main.c
  src/stream_rx.c
)

target_sources_ifdef(CONFIG_ENABLE_LC3 app PRIVATE src/lc3.c)

if(CONFIG_USE_CODEC_AUDIO_OUTPUT)
  target_sources(app PRIVATE src/hw_codec.c)
endif()

if(CONFIG_USE_USB_AUDIO_OUTPUT)
  include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
  target_sources(app PRIVATE src/usb.c)
endif()

if(CONFIG_USE_PIPEWIRE_AUDIO_OUTPUT)
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
    message(FATAL_ERROR "CONFIG_USE_PIPEWIRE_AUDIO_OUTPUT is only available on Linux")
  endif()
endif()
