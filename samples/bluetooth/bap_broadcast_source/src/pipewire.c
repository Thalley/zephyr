/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BAP Broadcast Source PipeWire capture extension (Zephyr side)
 *
 * Initialises command-line option handling and provides the interface used
 * by the LC3 encoder thread to obtain captured PCM data from the host-side
 * PipeWire capture stream.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include <zephyr/logging/log.h>

#include "pipewire.h"

LOG_MODULE_REGISTER(bap_broadcast_source_pipewire, CONFIG_LOG_DEFAULT_LEVEL);

/* Forward declarations for bottom-layer functions */
int pipewire_source_bottom_init(uint32_t src_rate, uint32_t dst_rate, const char *target,
				const char *latency);
bool pipewire_source_bottom_get(size_t channel, int16_t *buf, size_t num_samples);

static const char *pw_target = CONFIG_BROADCAST_SOURCE_PIPEWIRE_SOURCE;
static const char *pw_latency = CONFIG_BROADCAST_SOURCE_PIPEWIRE_LATENCY;
static uint32_t pw_dst_rate;

static void pipewire_options(void)
{
	static struct args_struct_t pw_args[] = {
		{
			.option = "bt-broadcast-source-pcm-source",
			.name = "node",
			.type = 's',
			.dest = (void *)&pw_target,
			.descript = "PipeWire source target for BAP Broadcast Source capture",
		},
		{
			.option = "bt-broadcast-source-pcm-latency",
			.name = "latency",
			.type = 's',
			.dest = (void *)&pw_latency,
			.descript = "PipeWire latency for BAP Broadcast Source capture",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(pw_args);
}

int pipewire_init(uint32_t dst_sample_rate_hz)
{
	const char *target = pw_target;
	const char *latency = pw_latency;

	pw_dst_rate = dst_sample_rate_hz;

	if ((target != NULL) && (target[0] == '\0')) {
		target = NULL;
	}

	if ((latency != NULL) && (latency[0] == '\0')) {
		latency = NULL;
	}

	return pipewire_source_bottom_init(PIPEWIRE_CAPTURE_RATE_HZ, dst_sample_rate_hz, target,
					   latency);
}

bool pipewire_get_samples(size_t channel, int16_t *buf, size_t num_samples)
{
	return pipewire_source_bottom_get(channel, buf, num_samples);
}

NATIVE_TASK(pipewire_options, PRE_BOOT_1, 1);
