/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UUT_BAP_BROADCAST_ASSISTANT_H_
#define UUT_BAP_BROADCAST_ASSISTANT_H_

#include <stdbool.h>

void set_skip_add_src(unsigned int nr_to_skip);
void set_add_src_encrypted(bool encrypted);

#endif /* UUT_BAP_BROADCAST_ASSISTANT_H_ */
