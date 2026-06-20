/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cmdline.h>
#include <errno.h>
#include <stddef.h>
#include <posix_native_task.h>

#include "pcm.h"

int bap_pipewire_bottom_init(const char *target, const char *latency);
int bap_pipewire_bottom_write(const int16_t *frame, size_t frame_size);

static const char *pcm_pipewire_target = CONFIG_BT_BAP_SHELL_PCM_PIPEWIRE_TARGET;
static const char *pcm_pipewire_latency = CONFIG_BT_BAP_SHELL_PCM_PIPEWIRE_LATENCY;

static void bap_pipewire_options(void)
{
	static struct args_struct_t bap_pipewire_options[] = {
		{
			.option = "bt-audio-pcm-sink",
			.name = "node",
			.type = 's',
			.dest = (void *)&pcm_pipewire_target,
			.descript =
				"PipeWire sink target for Bluetooth audio shell playback",
		},
		{
			.option = "bt-audio-pcm-latency",
			.name = "latency",
			.type = 's',
			.dest = (void *)&pcm_pipewire_latency,
			.descript =
				"PipeWire latency for Bluetooth audio shell playback",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(bap_pipewire_options);
}

static int bap_pipewire_init(void)
{
	const char *target = pcm_pipewire_target;
	const char *latency = pcm_pipewire_latency;

	if ((target != NULL) && (target[0] == '\0')) {
		target = NULL;
	}

	if ((latency != NULL) && (latency[0] == '\0')) {
		latency = NULL;
	}

	return bap_pipewire_bottom_init(target, latency);
}

static int bap_pipewire_write(const int16_t *frame, size_t frame_size)
{
	return bap_pipewire_bottom_write(frame, frame_size);
}

const struct bap_pcm_sink_backend bap_pipewire_pcm_backend = {
	.name = "PipeWire",
	.init = bap_pipewire_init,
	.write = bap_pipewire_write,
};

NATIVE_TASK(bap_pipewire_options, PRE_BOOT_1, 1);
