/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UUT_BAP_BROADCAST_ASSISTANT_H_
#define UUT_BAP_BROADCAST_ASSISTANT_H_

#include <stdbool.h>

struct bt_conn;

void set_skip_add_src(unsigned int nr_to_skip);
void set_add_src_encrypted(bool encrypted);
void defer_broadcast_code_response(bool defer);
void defer_bad_code_state(bool defer);
void reset_broadcast_assistant_mock(void);
struct bt_conn *report_deferred_bad_code_state(void);
struct bt_conn *complete_deferred_broadcast_code(int err);

#endif /* UUT_BAP_BROADCAST_ASSISTANT_H_ */
