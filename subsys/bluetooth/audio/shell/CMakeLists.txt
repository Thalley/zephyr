# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_link_libraries(subsys__bluetooth)

zephyr_library_sources_ifdef(
  CONFIG_BT_CCP_CALL_CONTROL_SERVER
  ccp_call_control_server.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CCP_CALL_CONTROL_CLIENT
  ccp_call_control_client.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_VCP_VOL_REND
  vcp_vol_rend.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_VCP_VOL_CTLR
  vcp_vol_ctlr.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_MICP_MIC_DEV
  micp_mic_dev.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_MICP_MIC_CTLR
  micp_mic_ctlr.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CSIP_SET_MEMBER
  csip_set_member.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CSIP_SET_COORDINATOR
  csip_set_coordinator.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_TBS
  tbs.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_TBS_CLIENT
  tbs_client.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_MPL
  mpl.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_MCC
  mcc.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_MCS
  media_controller.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_HAS_PRESET_SUPPORT
  has.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CAP_ACCEPTOR
  cap_acceptor.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CAP_INITIATOR
  cap_initiator.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CAP_COMMANDER
  cap_commander.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_CAP_HANDOVER
  cap_handover.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_HAS_CLIENT
  has_client.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_TMAP
  tmap.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_GMAP
  gmap.c
  )
# We use BT_BAP_STREAM as a common ground for audio, as that is set whenever
# any audio stream functionality is enabled.
zephyr_library_sources_ifdef(
  CONFIG_BT_BAP_STREAM
  bap.c
  )
zephyr_library_sources_ifdef(CONFIG_BT_BAP_SHELL_PCM pcm.c)
if(CONFIG_LIBLC3 AND CONFIG_USBD_AUDIO2_CLASS)
  zephyr_library_sources(bap_usb.c)
endif()
if(CONFIG_LIBLC3 AND CONFIG_BT_BAP_SHELL_PCM_BACKEND_PIPEWIRE)
  if(${CMAKE_HOST_SYSTEM_NAME} STREQUAL Linux)
    find_package(PkgConfig REQUIRED)
    pkg_search_module(PIPEWIRE REQUIRED libpipewire-0.3)
    list(TRANSFORM PIPEWIRE_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE PIPEWIRE_INCLUDE_DIRS_OPTION)
    target_compile_options(native_simulator INTERFACE "${PIPEWIRE_INCLUDE_DIRS_OPTION}"
                           ${PIPEWIRE_CFLAGS_OTHER})
    list(TRANSFORM PIPEWIRE_LIBRARIES PREPEND "-l" OUTPUT_VARIABLE PIPEWIRE_LIBRARIES_OPTION)
    target_link_options(native_simulator INTERFACE "${PIPEWIRE_LIBRARIES_OPTION}")
    zephyr_library_sources(pcm_pipewire.c)
    target_sources(native_simulator INTERFACE pcm_pipewire_bottom.c)
  else()
    message(FATAL_ERROR "CONFIG_BT_BAP_SHELL_PCM_BACKEND_PIPEWIRE is only available on Linux")
  endif()
endif()
zephyr_library_sources_ifdef(
  CONFIG_BT_BAP_SCAN_DELEGATOR
  bap_scan_delegator.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_BAP_BROADCAST_ASSISTANT
  bap_broadcast_assistant.c
  )
zephyr_library_sources_ifdef(
  CONFIG_BT_PBP
  pbp.c
  )
