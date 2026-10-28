#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

SIMULATION_ID="${BOARD_TS}_cap_broadcast_reception_encrypted"
VERBOSITY_LEVEL=2
NR_OF_DEVICES=4
EXECUTE_TIMEOUT=180

cd ${BSIM_OUT_PATH}/bin

printf "\n\n======== Running CAP commander encrypted broadcast reception test =========\n\n"

Execute ./bs_${BOARD_TS}_tests_bsim_bluetooth_audio_prj_conf \
  -v=${VERBOSITY_LEVEL} -s=${SIMULATION_ID} -d=0 \
  -testid=cap_commander_broadcast_reception_encrypted \
  -RealEncryption=1 -rs=46 -D=${NR_OF_DEVICES}

Execute ./bs_${BOARD_TS}_tests_bsim_bluetooth_audio_prj_conf \
  -v=${VERBOSITY_LEVEL} -s=${SIMULATION_ID} -d=1 -testid=cap_initiator_broadcast_encrypted \
  -RealEncryption=1 -rs=23 -D=${NR_OF_DEVICES}

Execute ./bs_${BOARD_TS}_tests_bsim_bluetooth_audio_prj_conf \
  -v=${VERBOSITY_LEVEL} -s=${SIMULATION_ID} -d=2 -testid=cap_acceptor_broadcast_reception \
  -RealEncryption=1 -rs=69 -D=${NR_OF_DEVICES} -start_offset=7e3

Execute ./bs_${BOARD_TS}_tests_bsim_bluetooth_audio_prj_conf \
  -v=${VERBOSITY_LEVEL} -s=${SIMULATION_ID} -d=3 -testid=cap_acceptor_broadcast_reception \
  -RealEncryption=1 -rs=87 -D=${NR_OF_DEVICES} -start_offset=7e3

# Simulation time should be larger than the WAIT_TIME in common.h
Execute ./bs_2G4_phy_v1 -v=${VERBOSITY_LEVEL} -s=${SIMULATION_ID} \
  -D=${NR_OF_DEVICES} -sim_length=60e6 $@



wait_for_background_jobs
