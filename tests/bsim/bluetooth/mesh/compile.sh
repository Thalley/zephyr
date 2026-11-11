#!/usr/bin/env bash
# Copyright 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

# Compile all the applications needed by the bsim tests in these subfolders

#set -x #uncomment this line for debugging
set -ue
: "${ZEPHYR_BASE:?ZEPHYR_BASE must be set to point to the zephyr root directory}"

source ${ZEPHYR_BASE}/tests/bsim/compile.source

app=tests/bsim/bluetooth/mesh compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_pst.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_gatt.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_gatt_separate.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_low_lat.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_workq_sys.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_multi_adv_sets.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file=extra_lpn_scan_on.conf compile
app=tests/bsim/bluetooth/mesh extra_conf_file="extra_gatt.conf;extra_workq_sys.conf" compile
app=tests/bsim/bluetooth/mesh extra_conf_file="extra_gatt.conf;extra_low_lat.conf" compile
app=tests/bsim/bluetooth/mesh extra_conf_file="extra_pst.conf;extra_gatt.conf" compile
app=tests/bsim/bluetooth/mesh \
  extra_conf_file="extra_gatt.conf;extra_multi_adv_sets.conf" compile
app=tests/bsim/bluetooth/mesh \
  extra_conf_file="extra_pst.conf;extra_gatt.conf;extra_workq_sys.conf" compile
wait_for_background_jobs
