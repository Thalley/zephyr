/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for bt_le_ext_adv_foreach()
 *
 * The advertising set storage differs depending on CONFIG_BT_EXT_ADV, so the
 * test suite is built both with extended advertising support enabled (using the
 * advertising set pool) and disabled (using the single legacy advertiser).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <host/hci_core.h>

/* Sentinel used to verify that the user data is passed on to the callback. */
static int user_data_sentinel = 1234;

#define MAX_ADV_SETS COND_CODE_1(CONFIG_BT_EXT_ADV, (CONFIG_BT_EXT_ADV_MAX_ADV_SET), (1))

/* State captured by the iteration callbacks. */
static struct {
	unsigned int call_count;
	struct bt_le_ext_adv *visited[MAX_ADV_SETS];
	bool unexpected_data;
	bool null_adv;
} cb_state;

static bool count_cb(struct bt_le_ext_adv *adv, void *data)
{
	if (adv == NULL) {
		cb_state.null_adv = true;
	}

	if (data != &user_data_sentinel) {
		cb_state.unexpected_data = true;
	}

	if (cb_state.call_count < ARRAY_SIZE(cb_state.visited)) {
		cb_state.visited[cb_state.call_count] = adv;
	}

	cb_state.call_count++;

	return true;
}

static bool stop_cb(struct bt_le_ext_adv *adv, void *data)
{
	(void)count_cb(adv, data);

	return false;
}

static void expect_visited(struct bt_le_ext_adv *adv)
{
	for (unsigned int i = 0; i < MIN(cb_state.call_count, ARRAY_SIZE(cb_state.visited)); i++) {
		if (cb_state.visited[i] == adv) {
			return;
		}
	}

	zassert_unreachable("Advertising set %p was not provided to the callback", adv);
}

static void test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)memset(&cb_state, 0, sizeof(cb_state));
}

static void common_setup(void)
{
	(void)memset(&bt_dev, 0, sizeof(bt_dev));
	bt_dev.id_count = 1;
	bt_dev.hci_version = BT_HCI_VERSION_5_0;
	/*
	 * Advertising parameters are rejected unless the identity address is set to something
	 * else than BT_ADDR_LE_ANY, so a static random address is used as the identity. The two
	 * most significant bits of a static random address are set (0xC0), and the remaining bits
	 * shall not all be equal, hence the 0x01.
	 */
	bt_dev.id_addr[0].type = BT_ADDR_LE_RANDOM;
	bt_dev.id_addr[0].a.val[0] = 0x01U;
	bt_dev.id_addr[0].a.val[5] = 0xC0U;
	atomic_set_bit(bt_dev.flags, BT_DEV_READY);
}

ZTEST_SUITE(bt_le_ext_adv_foreach, NULL, NULL, test_before, NULL, NULL);

/*
 * Passing a NULL callback is invalid and shall be rejected without touching any
 * advertising set.
 */
static ZTEST(bt_le_ext_adv_foreach, test_null_callback_returns_einval)
{
	int err;

	err = bt_le_ext_adv_foreach(NULL, &user_data_sentinel);

	zassert_equal(err, -EINVAL, "Unexpected return value %d", err);
}

/* A NULL callback is rejected regardless of the user data provided. */
static ZTEST(bt_le_ext_adv_foreach, test_null_callback_with_null_data_returns_einval)
{
	int err;

	err = bt_le_ext_adv_foreach(NULL, NULL);

	zassert_equal(err, -EINVAL, "Unexpected return value %d", err);
}

#if defined(CONFIG_BT_EXT_ADV)
/* The advertising sets created by the currently running test. */
static struct bt_le_ext_adv *created_advs[MAX_ADV_SETS];
static size_t created_adv_cnt;

static struct bt_le_ext_adv *create_adv_set(void)
{
	struct bt_le_ext_adv *adv = NULL;
	int err;

	err = bt_le_ext_adv_create(BT_LE_EXT_ADV_NCONN, NULL, &adv);

	zassert_ok(err, "Failed to create advertising set (%d)", err);
	zassert_not_null(adv, "Advertising set is NULL");
	zassert_true(created_adv_cnt < ARRAY_SIZE(created_advs), "Too many advertising sets");

	created_advs[created_adv_cnt++] = adv;

	return adv;
}

static void delete_adv_set(struct bt_le_ext_adv *adv)
{
	int err;

	err = bt_le_ext_adv_delete(adv);
	zassert_ok(err, "Failed to delete advertising set (%d)", err);

	for (size_t i = 0; i < created_adv_cnt; i++) {
		if (created_advs[i] == adv) {
			created_advs[i] = created_advs[created_adv_cnt - 1];
			created_adv_cnt--;
			break;
		}
	}
}

static void delete_all_adv_sets(void *fixture)
{
	ARG_UNUSED(fixture);

	while (created_adv_cnt > 0) {
		delete_adv_set(created_advs[created_adv_cnt - 1]);
	}
}

static void ext_adv_before(void *fixture)
{
	test_before(fixture);

	common_setup();
}

ZTEST_SUITE(bt_le_ext_adv_foreach_ext, NULL, NULL, ext_adv_before, delete_all_adv_sets, NULL);

/* Without any created advertising set the callback shall not be called. */
static ZTEST(bt_le_ext_adv_foreach_ext, test_no_adv_sets_returns_success)
{
	int err;

	err = bt_le_ext_adv_foreach(count_cb, &user_data_sentinel);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 0, "Callback called %u times",
		      cb_state.call_count);
}

/* Every created advertising set shall be provided to the callback exactly once. */
static ZTEST(bt_le_ext_adv_foreach_ext, test_all_adv_sets_are_provided)
{
	struct bt_le_ext_adv *advs[CONFIG_BT_EXT_ADV_MAX_ADV_SET];
	int err;

	ARRAY_FOR_EACH_PTR(advs, adv) {
		*adv = create_adv_set();
	}

	err = bt_le_ext_adv_foreach(count_cb, &user_data_sentinel);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, ARRAY_SIZE(advs), "Callback called %u times",
		      cb_state.call_count);
	zassert_false(cb_state.null_adv, "Callback called with a NULL advertising set");
	zassert_false(cb_state.unexpected_data, "Callback called with unexpected user data");

	ARRAY_FOR_EACH_PTR(advs, adv) {
		expect_visited(*adv);
	}
}

/* Deleted advertising sets shall no longer be provided to the callback. */
static ZTEST(bt_le_ext_adv_foreach_ext, test_deleted_adv_set_is_not_provided)
{
	struct bt_le_ext_adv *first;
	struct bt_le_ext_adv *second;
	int err;

	first = create_adv_set();
	second = create_adv_set();

	delete_adv_set(first);

	err = bt_le_ext_adv_foreach(count_cb, &user_data_sentinel);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
	expect_visited(second);
}

/*
 * A callback returning false shall stop the iteration and cause -ECANCELED to
 * be returned.
 */
static ZTEST(bt_le_ext_adv_foreach_ext, test_callback_stop_returns_ecanceled)
{
	int err;

	for (size_t i = 0; i < CONFIG_BT_EXT_ADV_MAX_ADV_SET; i++) {
		(void)create_adv_set();
	}

	err = bt_le_ext_adv_foreach(stop_cb, &user_data_sentinel);

	zassert_equal(err, -ECANCELED, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
}

/*
 * The user data pointer is optional and shall be forwarded to the callback
 * unmodified, including when it is NULL.
 */
static ZTEST(bt_le_ext_adv_foreach_ext, test_null_user_data_is_forwarded)
{
	int err;

	(void)create_adv_set();

	/* count_cb() flags any user data that is not the sentinel. */
	err = bt_le_ext_adv_foreach(count_cb, NULL);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
	zassert_true(cb_state.unexpected_data, "Callback did not receive the NULL user data");
}
#else /* !defined(CONFIG_BT_EXT_ADV) */
static void legacy_before(void *fixture)
{
	test_before(fixture);

	common_setup();
}

static void start_legacy_adv(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_NCONN, NULL, 0, NULL, 0);

	zassert_ok(err, "Failed to start legacy advertising (%d)", err);
}

ZTEST_SUITE(bt_le_ext_adv_foreach_legacy, NULL, NULL, legacy_before, NULL, NULL);

/*
 * The legacy advertising set always exists, but it shall only be provided to
 * the callback while it is advertising.
 */
static ZTEST(bt_le_ext_adv_foreach_legacy, test_inactive_adv_is_not_provided)
{
	int err;

	err = bt_le_ext_adv_foreach(count_cb, &user_data_sentinel);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 0, "Callback called %u times", cb_state.call_count);
}

/* The started legacy advertising set shall be provided to the callback. */
static ZTEST(bt_le_ext_adv_foreach_legacy, test_active_adv_is_provided)
{
	int err;

	start_legacy_adv();

	err = bt_le_ext_adv_foreach(count_cb, &user_data_sentinel);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
	zassert_false(cb_state.null_adv, "Callback called with a NULL advertising set");
	zassert_false(cb_state.unexpected_data, "Callback called with unexpected user data");
	expect_visited(&bt_dev.adv);
}

/*
 * A callback returning false shall stop the iteration and cause -ECANCELED to
 * be returned.
 */
static ZTEST(bt_le_ext_adv_foreach_legacy, test_callback_stop_returns_ecanceled)
{
	int err;

	start_legacy_adv();

	err = bt_le_ext_adv_foreach(stop_cb, &user_data_sentinel);

	zassert_equal(err, -ECANCELED, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
}

/*
 * The user data pointer is optional and shall be forwarded to the callback
 * unmodified, including when it is NULL.
 */
static ZTEST(bt_le_ext_adv_foreach_legacy, test_null_user_data_is_forwarded)
{
	int err;

	start_legacy_adv();

	/* count_cb() flags any user data that is not the sentinel. */
	err = bt_le_ext_adv_foreach(count_cb, NULL);

	zassert_ok(err, "Unexpected return value %d", err);
	zassert_equal(cb_state.call_count, 1, "Callback called %u times", cb_state.call_count);
	zassert_true(cb_state.unexpected_data, "Callback did not receive the NULL user data");
}
#endif /* defined(CONFIG_BT_EXT_ADV) */
