/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "audio.h"
#include "pcm.h"

LOG_MODULE_REGISTER(bap_pcm, CONFIG_BT_BAP_STREAM_LOG_LEVEL);

struct pcm_sink_stream {
	const void *stream;
	enum bt_audio_location chan_allocation;
};

struct decoded_sdu {
	int16_t right_frames[MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	int16_t left_frames[MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	size_t right_frames_cnt;
	size_t left_frames_cnt;
	size_t mono_frames_cnt;
	uint32_t ts;
};

static struct pcm_sink_stream pcm_sink_streams[CONFIG_BT_ISO_MAX_CHAN];
static const void *pcm_left_stream;
static const void *pcm_right_stream;
static struct decoded_sdu decoded_sdu;

static const struct bap_pcm_sink_backend *bap_pcm_backend(void)
{
#if defined(CONFIG_BT_BAP_SHELL_PCM_BACKEND_USB)
	return &bap_usb_pcm_backend;
#elif defined(CONFIG_BT_BAP_SHELL_PCM_BACKEND_PIPEWIRE)
	return &bap_pipewire_pcm_backend;
#else
	return NULL;
#endif
}

static struct pcm_sink_stream *pcm_sink_stream_find(const void *stream)
{
	for (size_t i = 0U; i < ARRAY_SIZE(pcm_sink_streams); i++) {
		if (pcm_sink_streams[i].stream == stream) {
			return &pcm_sink_streams[i];
		}
	}

	return NULL;
}

static struct pcm_sink_stream *pcm_sink_stream_alloc(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(pcm_sink_streams); i++) {
		if (pcm_sink_streams[i].stream == NULL) {
			return &pcm_sink_streams[i];
		}
	}

	return NULL;
}

static void pcm_sink_stream_select(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(pcm_sink_streams); i++) {
		const struct pcm_sink_stream *stream = &pcm_sink_streams[i];

		if (stream->stream == NULL) {
			continue;
		}

		if ((pcm_left_stream == NULL) &&
		    ((stream->chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0U)) {
			LOG_INF("Setting PCM left stream to %p", stream->stream);
			pcm_left_stream = stream->stream;
		}

		if ((pcm_right_stream == NULL) &&
		    ((stream->chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0U)) {
			LOG_INF("Setting PCM right stream to %p", stream->stream);
			pcm_right_stream = stream->stream;
		}
	}
}

static void pcm_send_frames(void)
{
	const struct bap_pcm_sink_backend *backend = bap_pcm_backend();
	const bool is_left_only =
		(decoded_sdu.right_frames_cnt == 0U) && (decoded_sdu.mono_frames_cnt == 0U);
	const bool is_right_only =
		(decoded_sdu.left_frames_cnt == 0U) && (decoded_sdu.mono_frames_cnt == 0U);
	const bool is_mono_only =
		(decoded_sdu.left_frames_cnt == 0U) && (decoded_sdu.right_frames_cnt == 0U);
	const bool is_single_channel = is_left_only || is_right_only || is_mono_only;
	const size_t frame_cnt = MAX(decoded_sdu.mono_frames_cnt,
				     MAX(decoded_sdu.left_frames_cnt, decoded_sdu.right_frames_cnt));

	if ((backend == NULL) || (backend->write == NULL)) {
		bap_pcm_clear_frames();
		return;
	}

	for (size_t i = 0U; i < frame_cnt; i++) {
		int16_t stereo_frame[LC3_MAX_NUM_SAMPLES_STEREO];
		const int16_t *right_frame = decoded_sdu.right_frames[i];
		const int16_t *left_frame = decoded_sdu.left_frames[i];
		const int16_t *mono_frame = decoded_sdu.left_frames[i];
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

		err = backend->write(stereo_frame, sizeof(stereo_frame));
		if (err != 0) {
			LOG_WRN("Failed to write PCM frame to %s: %d", backend->name, err);
			break;
		}
	}

	bap_pcm_clear_frames();
}

static bool ts_overflowed(uint32_t ts)
{
	return ((uint64_t)ts * 10U) < decoded_sdu.ts;
}

int bap_pcm_init(void)
{
	const struct bap_pcm_sink_backend *backend = bap_pcm_backend();

	if ((backend == NULL) || (backend->init == NULL)) {
		return 0;
	}

	return backend->init();
}

int bap_pcm_stream_started(const void *stream, enum bt_audio_location chan_allocation)
{
	struct pcm_sink_stream *entry;

	if (stream == NULL) {
		return -EINVAL;
	}

	entry = pcm_sink_stream_find(stream);
	if (entry == NULL) {
		entry = pcm_sink_stream_alloc();
		if (entry == NULL) {
			return -ENOMEM;
		}
	}

	entry->stream = stream;
	entry->chan_allocation = chan_allocation;

	pcm_sink_stream_select();

	if ((pcm_left_stream != stream) &&
	    ((chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0U)) {
		LOG_WRN("Multiple left streams started");
	}

	if ((pcm_right_stream != stream) &&
	    ((chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0U)) {
		LOG_WRN("Multiple right streams started");
	}

	return 0;
}

void bap_pcm_stream_stopped(const void *stream)
{
	struct pcm_sink_stream *entry = pcm_sink_stream_find(stream);

	if (entry == NULL) {
		return;
	}

	if (stream == pcm_left_stream) {
		LOG_INF("Clearing PCM left stream (%p)", stream);
		pcm_left_stream = NULL;
	}

	if (stream == pcm_right_stream) {
		LOG_INF("Clearing PCM right stream (%p)", stream);
		pcm_right_stream = NULL;
	}

	memset(entry, 0, sizeof(*entry));
	pcm_sink_stream_select();
}

bool bap_pcm_stream_matches(const void *stream, enum bt_audio_location chan_allocation)
{
	if (((chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0U) &&
	    (stream != pcm_left_stream)) {
		return false;
	}

	if (((chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0U) &&
	    (stream != pcm_right_stream)) {
		return false;
	}

	return true;
}

int bap_pcm_add_frame(const void *stream, enum bt_audio_location chan_allocation,
		      const int16_t *frame, size_t frame_size, uint32_t ts)
{
	const bool is_left = (chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0U;
	const bool is_right = (chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0U;
	const bool is_mono = chan_allocation == BT_AUDIO_LOCATION_MONO_AUDIO;
	const uint8_t ts_jitter_us = 100U;

	if ((stream == NULL) || (frame == NULL)) {
		return -EINVAL;
	}

	if (!bap_pcm_stream_matches(stream, chan_allocation)) {
		return -EIO;
	}

	if ((frame_size == 0U) || (frame_size > (LC3_MAX_NUM_SAMPLES_MONO * sizeof(int16_t)))) {
		return -EINVAL;
	}

	if (bt_audio_get_chan_count(chan_allocation) != 1) {
		return -EINVAL;
	}

	if (((is_left || is_right) && (decoded_sdu.mono_frames_cnt != 0U)) ||
	    (is_mono &&
	     ((decoded_sdu.left_frames_cnt != 0U) || (decoded_sdu.right_frames_cnt != 0U)))) {
		return -EINVAL;
	}

	if ((ts + ts_jitter_us) < decoded_sdu.ts && !ts_overflowed(ts)) {
		return -ENOEXEC;
	} else if ((ts > (decoded_sdu.ts + ts_jitter_us)) || ts_overflowed(ts)) {
		pcm_send_frames();
	} else {
		bool send = false;

		if (is_left && (decoded_sdu.left_frames_cnt > decoded_sdu.right_frames_cnt)) {
			send = true;
		} else if (is_right && (decoded_sdu.right_frames_cnt > decoded_sdu.left_frames_cnt)) {
			send = true;
		} else if (is_mono) {
			send = true;
		}

		if (send) {
			pcm_send_frames();
		}
	}

	if (is_left) {
		if (decoded_sdu.left_frames_cnt >= ARRAY_SIZE(decoded_sdu.left_frames)) {
			return -ENOMEM;
		}

		memcpy(decoded_sdu.left_frames[decoded_sdu.left_frames_cnt], frame, frame_size);
		decoded_sdu.left_frames_cnt++;
	} else if (is_right) {
		if (decoded_sdu.right_frames_cnt >= ARRAY_SIZE(decoded_sdu.right_frames)) {
			return -ENOMEM;
		}

		memcpy(decoded_sdu.right_frames[decoded_sdu.right_frames_cnt], frame, frame_size);
		decoded_sdu.right_frames_cnt++;
	} else if (is_mono) {
		if (decoded_sdu.mono_frames_cnt >= ARRAY_SIZE(decoded_sdu.left_frames)) {
			return -ENOMEM;
		}

		memcpy(decoded_sdu.left_frames[decoded_sdu.mono_frames_cnt], frame, frame_size);
		decoded_sdu.mono_frames_cnt++;
	} else {
		return -EINVAL;
	}

	decoded_sdu.ts = ts;

	return 0;
}

void bap_pcm_clear_frames(void)
{
	decoded_sdu.mono_frames_cnt = 0U;
	decoded_sdu.right_frames_cnt = 0U;
	decoded_sdu.left_frames_cnt = 0U;
	decoded_sdu.ts = 0U;
}
