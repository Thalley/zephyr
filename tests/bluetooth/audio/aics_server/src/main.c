/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/aics.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "gatt.h"

DEFINE_FFF_GLOBALS;

static K_SEM_DEFINE(workqueue_blocked, 0, 1);
static K_SEM_DEFINE(workqueue_continue, 0, 1);
static K_SEM_DEFINE(callbacks_complete, 0, 3);

static struct bt_aics *aics_inst;
static struct bt_aics *aics_inst_without_callbacks;
static bool state_cb_on_sys_work_q;
static int state_cb_err;
static int gain_setting_cb_err;
static int status_cb_err;

static void block_workqueue(struct k_work *work)
{
	ARG_UNUSED(work);

	k_sem_give(&workqueue_blocked);
	(void)k_sem_take(&workqueue_continue, K_FOREVER);
}

static K_WORK_DEFINE(workqueue_blocker, block_workqueue);

static void state_cb(struct bt_aics *inst, int err, int8_t gain, uint8_t mute,
		     uint8_t mode)
{
	ARG_UNUSED(inst);
	ARG_UNUSED(gain);
	ARG_UNUSED(mute);
	ARG_UNUSED(mode);

	state_cb_err = err;
	state_cb_on_sys_work_q = k_current_get() == k_work_queue_thread_get(&k_sys_work_q);
	k_sem_give(&callbacks_complete);
}

static void gain_setting_cb(struct bt_aics *inst, int err, uint8_t units, int8_t minimum,
			    int8_t maximum)
{
	ARG_UNUSED(inst);
	ARG_UNUSED(units);
	ARG_UNUSED(minimum);
	ARG_UNUSED(maximum);

	gain_setting_cb_err = err;
	k_sem_give(&callbacks_complete);
}

static void status_cb(struct bt_aics *inst, int err, bool active)
{
	ARG_UNUSED(inst);
	ARG_UNUSED(active);

	status_cb_err = err;
	k_sem_give(&callbacks_complete);
}

static void *aics_server_test_setup(void)
{
	static struct bt_aics_cb callbacks = {
		.state = state_cb,
		.gain_setting = gain_setting_cb,
		.status = status_cb,
	};
	static struct bt_aics_register_param param = {
		.gain = 0,
		.mute = BT_AICS_STATE_UNMUTED,
		.gain_mode = BT_AICS_MODE_MANUAL,
		.units = 1,
		.min_gain = -10,
		.max_gain = 10,
		.type = BT_AICS_INPUT_TYPE_ANALOG,
		.status = true,
		.desc_writable = true,
		.description = "Input",
		.cb = &callbacks,
	};
	static struct bt_aics_register_param param_without_callbacks = {
		.gain = 0,
		.mute = BT_AICS_STATE_UNMUTED,
		.gain_mode = BT_AICS_MODE_MANUAL,
		.units = 1,
		.min_gain = -10,
		.max_gain = 10,
		.type = BT_AICS_INPUT_TYPE_ANALOG,
		.status = true,
		.description = "Input without callbacks",
	};
	int err;

	mock_bt_gatt_init();

	aics_inst = bt_aics_free_instance_get();
	zassert_not_null(aics_inst);

	err = bt_aics_register(aics_inst, &param);
	zassert_ok(err);

	aics_inst_without_callbacks = bt_aics_free_instance_get();
	zassert_not_null(aics_inst_without_callbacks);

	err = bt_aics_register(aics_inst_without_callbacks, &param_without_callbacks);
	zassert_ok(err);

	return NULL;
}

static void aics_server_test_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	mock_bt_gatt_cleanup();
}

ZTEST_SUITE(aics_server_test_suite, NULL, aics_server_test_setup, NULL,
	    aics_server_test_teardown, NULL);

ZTEST(aics_server_test_suite, test_server_callback_gating)
{
	int results[12];
	int expected[ARRAY_SIZE(results)] = {
		0,
		-EBUSY,
		-EBUSY,
		-EBUSY,
		-EBUSY,
		-EBUSY,
		0,
		-EBUSY,
		0,
		-EBUSY,
		0,
		0,
	};
	int err;

	err = k_work_submit(&workqueue_blocker);
	zassert_true(err >= 0, "Failed to submit workqueue blocker: %d", err);

	err = k_sem_take(&workqueue_blocked, K_SECONDS(5));
	if (err != 0) {
		k_sem_give(&workqueue_continue);
		zassert_ok(err, "Workqueue blocker did not start");
	}

	results[0] = bt_aics_mute(aics_inst);
	results[1] = bt_aics_mute(aics_inst);
	results[2] = bt_aics_state_get(aics_inst);
	results[3] = bt_aics_disable_mute(aics_inst);
	results[4] = bt_aics_gain_set_manual_only(aics_inst);
	results[5] = bt_aics_gain_set_auto_only(aics_inst);
	results[6] = bt_aics_gain_setting_get(aics_inst);
	results[7] = bt_aics_gain_setting_get(aics_inst);
	results[8] = bt_aics_deactivate(aics_inst);
	results[9] = bt_aics_activate(aics_inst);
	results[10] = bt_aics_mute(aics_inst_without_callbacks);
	results[11] = bt_aics_state_get(aics_inst_without_callbacks);

	k_sem_give(&workqueue_continue);

	for (size_t i = 0; i < ARRAY_SIZE(results); i++) {
		zassert_equal(results[i], expected[i], "Unexpected result at index %zu", i);
	}

	for (size_t i = 0; i < 3U; i++) {
		err = k_sem_take(&callbacks_complete, K_SECONDS(5));
		zassert_ok(err, "AICS callback did not run");
	}

	zassert_equal(state_cb_err, 0);
	zassert_true(state_cb_on_sys_work_q, "State callback did not run on the system workqueue");
	zassert_equal(gain_setting_cb_err, 0);
	zassert_equal(status_cb_err, 0);
}
