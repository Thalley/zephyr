/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/sys/slist.h>
#include <zephyr/ztest_assert.h>

#include "bap_broadcast_assistant.h"
#include "test_common.h"

static sys_slist_t broadcast_assistant_cbs = SYS_SLIST_STATIC_INIT(&broadcast_assistant_cbs);

/* when > 0 immediately return from the add_src callback the specified number of times
 * This allows us to test the CAP cancel command by not successfully sending all the
 * requests, so that we can cancel before the CAP commander implementation is done
 * with the procedure.
 * This is based on the fact that we are not actually sending any requests on air, and
 * that we are using this function as a synchronous function, rather than an asynchronous
 * function.
 */
static unsigned int add_src_skip;

void set_skip_add_src(unsigned int setting)
{
	add_src_skip = setting;
}

/* When true, added sources request a broadcast code and are not BIS synced until the broadcast
 * code has been set
 */
static bool add_src_encrypted;

void set_add_src_encrypted(bool encrypted)
{
	add_src_encrypted = encrypted;
}

static bool defer_broadcast_code_rsp;
static bool defer_bad_code_state_notification;
static struct bt_conn *deferred_broadcast_code_conn;
static struct bt_conn *deferred_bad_code_conn;
static struct bt_bap_scan_delegator_recv_state deferred_bad_code_recv_state;

void defer_broadcast_code_response(bool defer)
{
	defer_broadcast_code_rsp = defer;
}

void defer_bad_code_state(bool defer)
{
	defer_bad_code_state_notification = defer;
}

static void broadcast_assistant_broadcast_code_cb(struct bt_conn *conn, int err)
{
	struct bt_bap_broadcast_assistant_cb *listener, *next;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&broadcast_assistant_cbs, listener, next, _node) {
		if (listener->broadcast_code != NULL) {
			listener->broadcast_code(conn, err);
		}
	}
}

static void broadcast_assistant_recv_state_cb(
	struct bt_conn *conn, const struct bt_bap_scan_delegator_recv_state *state)
{
	struct bt_bap_broadcast_assistant_cb *listener, *next;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&broadcast_assistant_cbs, listener, next, _node) {
		if (listener->recv_state != NULL) {
			listener->recv_state(conn, 0, state);
		}
	}
}

struct bt_conn *report_deferred_bad_code_state(void)
{
	struct bt_conn *conn = deferred_bad_code_conn;

	zassert_not_null(conn, "No deferred BAD_CODE state");
	broadcast_assistant_recv_state_cb(conn, &deferred_bad_code_recv_state);
	deferred_bad_code_conn = NULL;

	return conn;
}

struct bt_conn *complete_deferred_broadcast_code(int err)
{
	struct bt_conn *conn = deferred_broadcast_code_conn;

	zassert_not_null(conn, "No deferred Set Broadcast_Code response");
	broadcast_assistant_broadcast_code_cb(conn, err);
	deferred_broadcast_code_conn = NULL;

	return conn;
}

struct bap_broadcast_assistant_recv_state_info {
	uint8_t src_id;
	/** Cached PAST available */
	bool past_avail;
	uint8_t adv_sid;
	uint32_t broadcast_id;
	bt_addr_le_t addr;
};

struct bap_broadcast_assistant_instance {
	struct bt_conn *conn;
	struct bap_broadcast_assistant_recv_state_info recv_states;
	/*
	 * the following are not part of the broadcast_assistant instance, but adding them allow us
	 * to easily check pa_sync and bis_sync states
	 */
	enum bt_bap_pa_state pa_sync_state;
	bool broadcast_code_requested;
	uint8_t num_subgroups;
	struct bt_bap_bass_subgroup subgroups[CONFIG_BT_BAP_BASS_MAX_SUBGROUPS];
};

static struct bap_broadcast_assistant_instance broadcast_assistants[CONFIG_BT_MAX_CONN];
static uint8_t max_src_id;

void reset_broadcast_assistant_mock(void)
{
	(void)memset(broadcast_assistants, 0, sizeof(broadcast_assistants));
	max_src_id = 0U;
	add_src_skip = 0U;
	add_src_encrypted = false;
	defer_broadcast_code_rsp = false;
	defer_bad_code_state_notification = false;
	deferred_broadcast_code_conn = NULL;
	deferred_bad_code_conn = NULL;
	(void)memset(&deferred_bad_code_recv_state, 0, sizeof(deferred_bad_code_recv_state));
}

static struct bap_broadcast_assistant_instance *inst_by_conn(struct bt_conn *conn)
{
	struct bap_broadcast_assistant_instance *inst;

	zassert_not_null(conn, "conn is NULL");

	inst = &broadcast_assistants[bt_conn_index(conn)];

	return inst;
}

int bt_bap_broadcast_assistant_register_cb(struct bt_bap_broadcast_assistant_cb *cb)
{
	struct bt_bap_broadcast_assistant_cb *tmp;

	if (cb == NULL) {
		return -EINVAL;
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&broadcast_assistant_cbs, tmp, _node) {
		if (tmp == cb) {
			return -EALREADY;
		}
	}

	sys_slist_append(&broadcast_assistant_cbs, &cb->_node);

	return 0;
}

int bt_bap_broadcast_assistant_unregister_cb(struct bt_bap_broadcast_assistant_cb *cb)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	if (!sys_slist_find_and_remove(&broadcast_assistant_cbs, &cb->_node)) {
		return -EALREADY;
	}

	return 0;
}

int bt_bap_broadcast_assistant_add_src(struct bt_conn *conn,
				       const struct bt_bap_broadcast_assistant_add_src_param *param)
{
	struct bap_broadcast_assistant_instance *inst;
	struct bt_bap_scan_delegator_recv_state state = {0};
	struct bt_bap_broadcast_assistant_cb *listener, *next;

	if (add_src_skip != 0) {
		add_src_skip--;

		return 0;
	}

	/* Note that proper parameter checking is done in the caller */
	zassert_not_null(conn, "conn is NULL");
	zassert_not_null(param, "param is NULL");

	inst = inst_by_conn(conn);
	zassert_not_null(inst, "inst is NULL");

	max_src_id++;
	inst->recv_states.past_avail = false;

	state.src_id = inst->recv_states.src_id = max_src_id;
	state.adv_sid = inst->recv_states.adv_sid = param->adv_sid;
	state.broadcast_id = inst->recv_states.broadcast_id = param->broadcast_id;
	state.pa_sync_state = inst->pa_sync_state =
		param->pa_sync ? BT_BAP_PA_STATE_SYNCED : BT_BAP_PA_STATE_NOT_SYNCED;
	state.num_subgroups = inst->num_subgroups = param->num_subgroups;

	inst->broadcast_code_requested = add_src_encrypted;
	state.encrypt_state = add_src_encrypted ? BT_BAP_BIG_ENC_STATE_BCODE_REQ
						: BT_BAP_BIG_ENC_STATE_NO_ENC;

	for (size_t i = 0U; i < param->num_subgroups; i++) {
		inst->subgroups[i].bis_sync = param->subgroups[i].bis_sync;
		/* Encrypted sources are not BIS synced until the broadcast code has been set */
		state.subgroups[i].bis_sync = add_src_encrypted ? 0U : param->subgroups[i].bis_sync;
	}

	bt_addr_le_copy(&inst->recv_states.addr, &param->addr);
	bt_addr_le_copy(&state.addr, &param->addr);

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&broadcast_assistant_cbs, listener, next, _node) {
		if (listener->add_src != NULL) {
			listener->add_src(conn, 0);
		}
		if (listener->recv_state != NULL) {
			listener->recv_state(conn, 0, &state);
		}
	}

	return 0;
}

int bt_bap_broadcast_assistant_mod_src(struct bt_conn *conn,
				       const struct bt_bap_broadcast_assistant_mod_src_param *param)
{
	struct bap_broadcast_assistant_instance *inst;
	struct bt_bap_scan_delegator_recv_state state;
	struct bt_bap_broadcast_assistant_cb *listener, *next;

	zassert_not_null(conn, "conn is NULL");
	zassert_not_null(param, "param is NULL");

	inst = inst_by_conn(conn);
	zassert_not_null(inst, "inst is NULL");

	state.pa_sync_state = param->pa_sync ? BT_BAP_PA_STATE_SYNCED : BT_BAP_PA_STATE_NOT_SYNCED;
	state.src_id = param->src_id;
	inst->recv_states.src_id = param->src_id;
	inst->pa_sync_state = param->pa_sync;

	state.num_subgroups = param->num_subgroups;
	inst->num_subgroups = param->num_subgroups;
	for (size_t i = 0U; i < param->num_subgroups; i++) {
		state.subgroups[i].bis_sync = param->subgroups[i].bis_sync;
		inst->subgroups[i].bis_sync = param->subgroups[i].bis_sync;
	}
		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&broadcast_assistant_cbs, listener, next, _node) {
			if (listener->mod_src != NULL) {
				listener->mod_src(conn, 0);
			}
			if (listener->recv_state != NULL) {
				listener->recv_state(conn, 0, &state);
			}
		}

	return 0;
}

int bt_bap_broadcast_assistant_rem_src(struct bt_conn *conn, uint8_t src_id)
{
	struct bap_broadcast_assistant_instance *inst;
	struct bt_bap_broadcast_assistant_cb *listener, *next;

	zassert_not_null(conn, "conn is NULL");

	inst = inst_by_conn(conn);
	zassert_equal(src_id, inst->recv_states.src_id, "Invalid src_id");
	zassert_equal(BT_BAP_PA_STATE_NOT_SYNCED, inst->pa_sync_state, "Invalid sync state");
	for (int i = 0; i < inst->num_subgroups; i++) {
		zassert_equal(0, inst->subgroups[i].bis_sync);
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&broadcast_assistant_cbs, listener, next, _node) {
		if (listener->rem_src != NULL) {
			listener->rem_src(conn, 0);
		}
		if (listener->recv_state_removed != NULL) {
			listener->recv_state_removed(conn, src_id);
		}
	}

	return 0;
}

int bt_bap_broadcast_assistant_set_broadcast_code(
	struct bt_conn *conn, uint8_t src_id,
	const uint8_t broadcast_code[BT_ISO_BROADCAST_CODE_SIZE])
{
	struct bap_broadcast_assistant_instance *inst;
	struct bt_bap_scan_delegator_recv_state state = {0};
	int err;

	zassert_not_null(conn, "conn is NULL");

	inst = inst_by_conn(conn);
	zassert_not_null(inst, "inst is NULL");

	if (inst->broadcast_code_requested) {
		zassert_equal(src_id, inst->recv_states.src_id, "Invalid src_id");
	} else {
		zassert_equal(src_id, RANDOM_SRC_ID, "Invalid src_id");
	}

	err = strncmp((const char *)broadcast_code, BROADCAST_CODE, sizeof(BROADCAST_CODE));
	zassert_equal(0, err, "Unexpected broadcast code");

	if (defer_broadcast_code_rsp) {
		zassert_is_null(deferred_broadcast_code_conn,
				"A Set Broadcast_Code response is pending");
		deferred_broadcast_code_conn = conn;
	} else {
		broadcast_assistant_broadcast_code_cb(conn, 0);
	}

	if (inst->broadcast_code_requested) {
		/* Now that the broadcast code has been set, the source can be BIS synced */
		state.src_id = inst->recv_states.src_id;
		state.adv_sid = inst->recv_states.adv_sid;
		state.broadcast_id = inst->recv_states.broadcast_id;
		bt_addr_le_copy(&state.addr, &inst->recv_states.addr);
		state.pa_sync_state = inst->pa_sync_state;
		state.encrypt_state = BT_BAP_BIG_ENC_STATE_BAD_CODE;
		state.num_subgroups = inst->num_subgroups;
		for (size_t i = 0U; i < inst->num_subgroups; i++) {
			state.subgroups[i].bis_sync = inst->subgroups[i].bis_sync;
		}

		if (defer_broadcast_code_rsp && defer_bad_code_state_notification) {
			zassert_is_null(deferred_bad_code_conn, "A BAD_CODE state is pending");
			deferred_bad_code_conn = conn;
			deferred_bad_code_recv_state = state;
		} else if (!defer_broadcast_code_rsp) {
			inst->broadcast_code_requested = false;
			state.encrypt_state = BT_BAP_BIG_ENC_STATE_DEC;
			broadcast_assistant_recv_state_cb(conn, &state);
		}
	}

	return 0;
}
