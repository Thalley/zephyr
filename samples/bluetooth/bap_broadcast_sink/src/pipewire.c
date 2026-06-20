/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth BAP Broadcast Sink sample PipeWire extension
 *
 * This file handles all the PipeWire related functionality for audio output
 * in the BAP Broadcast Sink sample.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "lc3.h"
#include "pipewire.h"

LOG_MODULE_REGISTER(bap_broadcast_sink_pipewire, CONFIG_LOG_DEFAULT_LEVEL);

/* Forward declarations for bottom-layer functions */
int pipewire_bottom_init(const char *target, const char *latency);
int pipewire_bottom_write(const int16_t *frame, size_t frame_size);

static const char *pw_target = CONFIG_BROADCAST_SINK_PIPEWIRE_TARGET;
static const char *pw_latency = CONFIG_BROADCAST_SINK_PIPEWIRE_LATENCY;

struct decoded_sdu {
	int16_t right_frames[CONFIG_MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	int16_t left_frames[CONFIG_MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	size_t right_frames_cnt;
	size_t left_frames_cnt;
	size_t mono_frames_cnt;
	uint32_t ts;
};

static struct decoded_sdu pw_decoded_sdu;

static void pipewire_options(void)
{
	static struct args_struct_t pw_args[] = {
		{
			.option = "bt-broadcast-sink-pcm-sink",
			.name = "node",
			.type = 's',
			.dest = (void *)&pw_target,
			.descript = "PipeWire sink target for BAP Broadcast Sink playback",
		},
		{
			.option = "bt-broadcast-sink-pcm-latency",
			.name = "latency",
			.type = 's',
			.dest = (void *)&pw_latency,
			.descript = "PipeWire latency for BAP Broadcast Sink playback",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(pw_args);
}

static void pipewire_send_frames_to_pipewire(void)
{
	const bool is_left_only =
		pw_decoded_sdu.right_frames_cnt == 0U && pw_decoded_sdu.mono_frames_cnt == 0U;
	const bool is_right_only =
		pw_decoded_sdu.left_frames_cnt == 0U && pw_decoded_sdu.mono_frames_cnt == 0U;
	const bool is_mono_only =
		pw_decoded_sdu.left_frames_cnt == 0U && pw_decoded_sdu.right_frames_cnt == 0U;
	const bool is_single_channel = is_left_only || is_right_only || is_mono_only;
	const size_t frame_cnt =
		MAX(pw_decoded_sdu.mono_frames_cnt,
		    MAX(pw_decoded_sdu.left_frames_cnt, pw_decoded_sdu.right_frames_cnt));

	for (size_t i = 0U; i < frame_cnt; i++) {
		int16_t stereo_frame[LC3_MAX_NUM_SAMPLES_STEREO];
		const int16_t *right_frame = pw_decoded_sdu.right_frames[i];
		const int16_t *left_frame = pw_decoded_sdu.left_frames[i];
		const int16_t *mono_frame = pw_decoded_sdu.left_frames[i]; /* use left as mono */
		int err;

		for (size_t j = 0U; j < LC3_MAX_NUM_SAMPLES_MONO; j++) {
			if (is_single_channel) {
				int16_t sample = 0;

				if (is_left_only) {
					sample = left_frame[j];
				} else if (is_right_only) {
					sample = right_frame[j];
				} else if (is_mono_only) {
					sample = mono_frame[j];
				}

				stereo_frame[j * 2U] = sample;
				stereo_frame[(j * 2U) + 1U] = sample;
			} else {
				stereo_frame[j * 2U] = left_frame[j];
				stereo_frame[(j * 2U) + 1U] = right_frame[j];
			}
		}

		err = pipewire_bottom_write(stereo_frame, sizeof(stereo_frame));
		if (err != 0) {
			LOG_WRN("Failed to write PCM frame to PipeWire: %d", err);
			break;
		}
	}

	pipewire_clear_frames_to_pipewire();
}

static bool ts_overflowed(uint32_t ts)
{
	/* If the timestamp is a factor of 10 in difference, assume TS overflowed */
	return ((uint64_t)ts * 10U < pw_decoded_sdu.ts);
}

int pipewire_add_frame_to_pipewire(enum bt_audio_location chan_allocation, const int16_t *frame,
				   size_t frame_size, uint32_t ts)
{
	const bool is_left = (chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0U;
	const bool is_right = (chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0U;
	const bool is_mono = chan_allocation == BT_AUDIO_LOCATION_MONO_AUDIO;
	const uint8_t ts_jitter_us = 100U;

	if (frame_size > LC3_MAX_NUM_SAMPLES_MONO * sizeof(int16_t) || frame_size == 0U) {
		LOG_DBG("Invalid frame size %zu", frame_size);
		return -EINVAL;
	}

	if (bt_audio_get_chan_count(chan_allocation) != 1) {
		LOG_DBG("Invalid channel allocation %d", chan_allocation);
		return -EINVAL;
	}

	if (((is_left || is_right) && pw_decoded_sdu.mono_frames_cnt != 0U) ||
	    (is_mono && (pw_decoded_sdu.left_frames_cnt != 0U ||
			pw_decoded_sdu.right_frames_cnt != 0U))) {
		LOG_WRN("Cannot mix mono with left/right: %s: %u | %u | %u",
			is_left ? "left" : is_right ? "right" : "mono",
			pw_decoded_sdu.mono_frames_cnt, pw_decoded_sdu.left_frames_cnt,
			pw_decoded_sdu.right_frames_cnt);
		return -EINVAL;
	}

	if ((ts + ts_jitter_us) < pw_decoded_sdu.ts && !ts_overflowed(ts)) {
		/* Old data, discard */
		return -ENOEXEC;
	} else if (ts > (pw_decoded_sdu.ts + ts_jitter_us) || ts_overflowed(ts)) {
		/* New data: flush existing frames first */
		pipewire_send_frames_to_pipewire();
	} else { /* same timestamp */
		bool send = false;

		if (is_left && pw_decoded_sdu.left_frames_cnt > pw_decoded_sdu.right_frames_cnt) {
			send = true;
		} else if (is_right &&
			   pw_decoded_sdu.right_frames_cnt > pw_decoded_sdu.left_frames_cnt) {
			send = true;
		} else if (is_mono) {
			send = true;
		}

		if (send) {
			pipewire_send_frames_to_pipewire();
		}
	}

	if (is_left) {
		if (pw_decoded_sdu.left_frames_cnt >= ARRAY_SIZE(pw_decoded_sdu.left_frames)) {
			LOG_WRN("No room for more left frames");
			return -ENOMEM;
		}

		(void)memcpy(pw_decoded_sdu.left_frames[pw_decoded_sdu.left_frames_cnt], frame,
			     frame_size);
		pw_decoded_sdu.left_frames_cnt++;
	} else if (is_right) {
		if (pw_decoded_sdu.right_frames_cnt >= ARRAY_SIZE(pw_decoded_sdu.right_frames)) {
			LOG_WRN("No room for more right frames");
			return -ENOMEM;
		}

		(void)memcpy(pw_decoded_sdu.right_frames[pw_decoded_sdu.right_frames_cnt], frame,
			     frame_size);
		pw_decoded_sdu.right_frames_cnt++;
	} else if (is_mono) {
		/* Use left_frames array for mono */
		if (pw_decoded_sdu.mono_frames_cnt >= ARRAY_SIZE(pw_decoded_sdu.left_frames)) {
			LOG_WRN("No room for more mono frames");
			return -ENOMEM;
		}

		(void)memcpy(pw_decoded_sdu.left_frames[pw_decoded_sdu.mono_frames_cnt], frame,
			     frame_size);
		pw_decoded_sdu.mono_frames_cnt++;
	} else {
		LOG_DBG("Unsupported channel allocation %d", chan_allocation);
		return -EINVAL;
	}

	pw_decoded_sdu.ts = ts;

	return 0;
}

void pipewire_clear_frames_to_pipewire(void)
{
	pw_decoded_sdu.mono_frames_cnt = 0U;
	pw_decoded_sdu.right_frames_cnt = 0U;
	pw_decoded_sdu.left_frames_cnt = 0U;
	pw_decoded_sdu.ts = 0U;
}

int pipewire_init(void)
{
	const char *target = pw_target;
	const char *latency = pw_latency;

	if ((target != NULL) && (target[0] == '\0')) {
		target = NULL;
	}

	if ((latency != NULL) && (latency[0] == '\0')) {
		latency = NULL;
	}

	return pipewire_bottom_init(target, latency);
}

NATIVE_TASK(pipewire_options, PRE_BOOT_1, 1);
