/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include "audio.h"

#define BAP_PIPEWIRE_CHANNELS 2U
#define BAP_PIPEWIRE_STRIDE   (BAP_PIPEWIRE_CHANNELS * sizeof(int16_t))
#define BAP_PIPEWIRE_BUFFER_SIZE                                                            \
	((CONFIG_BT_BAP_SHELL_PCM_PIPEWIRE_BUFFER_MS * USB_SAMPLE_RATE *                        \
	  BAP_PIPEWIRE_STRIDE) / MSEC_PER_SEC)

struct bap_pipewire_state {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	pthread_mutex_t lock;
	uint8_t *buffer;
	size_t buffer_size;
	size_t read_idx;
	size_t write_idx;
	size_t fill_level;
	bool initialized;
};

static struct bap_pipewire_state pw_state;

static size_t pipewire_copy_from_ring(uint8_t *dst, size_t size)
{
	size_t copied = 0U;

	pthread_mutex_lock(&pw_state.lock);

	while ((copied < size) && (pw_state.fill_level > 0U)) {
		const size_t first = MIN(size - copied,
					 MIN(pw_state.fill_level, pw_state.buffer_size - pw_state.read_idx));

		memcpy(&dst[copied], &pw_state.buffer[pw_state.read_idx], first);
		pw_state.read_idx = (pw_state.read_idx + first) % pw_state.buffer_size;
		pw_state.fill_level -= first;
		copied += first;
	}

	pthread_mutex_unlock(&pw_state.lock);

	return copied;
}

static void pipewire_stream_process(void *userdata)
{
	struct pw_buffer *buffer;
	struct spa_buffer *spa_buf;
	struct spa_data *data;

	ARG_UNUSED(userdata);

	buffer = pw_stream_dequeue_buffer(pw_state.stream);
	if (buffer == NULL) {
		return;
	}

	spa_buf = buffer->buffer;
	data = &spa_buf->datas[0];

	if ((data->data == NULL) || (data->maxsize == 0U)) {
		pw_stream_queue_buffer(pw_state.stream, buffer);
		return;
	}

	memset(data->data, 0, data->maxsize);
	(void)pipewire_copy_from_ring(data->data, data->maxsize);

	data->chunk->offset = 0U;
	data->chunk->stride = BAP_PIPEWIRE_STRIDE;
	data->chunk->size = data->maxsize;

	pw_stream_queue_buffer(pw_state.stream, buffer);
}

static void pipewire_stream_state_changed(void *userdata, enum pw_stream_state old_state,
					  enum pw_stream_state state, const char *error)
{
	ARG_UNUSED(userdata);
	ARG_UNUSED(old_state);
	ARG_UNUSED(error);

	if ((state == PW_STREAM_STATE_ERROR) || (state == PW_STREAM_STATE_UNCONNECTED)) {
		pw_state.initialized = false;
	}
}

static const struct pw_stream_events pipewire_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pipewire_stream_state_changed,
	.process = pipewire_stream_process,
};

int bap_pipewire_bottom_init(const char *target, const char *latency)
{
	struct pw_properties *props;
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_S16_LE,
		.channels = BAP_PIPEWIRE_CHANNELS,
		.rate = USB_SAMPLE_RATE,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
	};
	const struct spa_pod *params[1];
	struct spa_pod_builder builder;
	uint8_t buffer[256];
	int err;

	if (pw_state.initialized) {
		return 0;
	}

	pw_init(NULL, NULL);

	pw_state.buffer = calloc(1, BAP_PIPEWIRE_BUFFER_SIZE);
	if (pw_state.buffer == NULL) {
		return -ENOMEM;
	}

	pw_state.buffer_size = BAP_PIPEWIRE_BUFFER_SIZE;
	pthread_mutex_init(&pw_state.lock, NULL);

	pw_state.loop = pw_thread_loop_new("zephyr-bt-audio", NULL);
	if (pw_state.loop == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
				  PW_KEY_MEDIA_ROLE, "Communication", NULL);
	if (props == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	if (target != NULL) {
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
	}

	if (latency != NULL) {
		pw_properties_set(props, PW_KEY_NODE_LATENCY, latency);
	}

	pw_state.stream = pw_stream_new_simple(pw_thread_loop_get_loop(pw_state.loop),
					       "zephyr-bt-audio-shell", props,
					       &pipewire_stream_events, NULL);
	if (pw_state.stream == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

	err = pw_thread_loop_start(pw_state.loop);
	if (err < 0) {
		err = -EIO;
		goto fail;
	}

	err = pw_stream_connect(pw_state.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
					PW_STREAM_FLAG_RT_PROCESS,
				params, ARRAY_SIZE(params));
	if (err < 0) {
		err = -EIO;
		goto fail;
	}

	pw_state.initialized = true;

	return 0;

fail:
	if (pw_state.stream != NULL) {
		pw_stream_destroy(pw_state.stream);
		pw_state.stream = NULL;
	}

	if (pw_state.loop != NULL) {
		pw_thread_loop_destroy(pw_state.loop);
		pw_state.loop = NULL;
	}

	if (pw_state.buffer != NULL) {
		free(pw_state.buffer);
		pw_state.buffer = NULL;
	}

	pthread_mutex_destroy(&pw_state.lock);
	pw_state.buffer_size = 0U;

	return err;
}

int bap_pipewire_bottom_write(const int16_t *frame, size_t frame_size)
{
	const uint8_t *src = (const uint8_t *)frame;
	size_t remaining = frame_size;

	if (!pw_state.initialized) {
		return -EIO;
	}

	pthread_mutex_lock(&pw_state.lock);

	if ((pw_state.buffer_size - pw_state.fill_level) < frame_size) {
		pthread_mutex_unlock(&pw_state.lock);
		return -ENOMEM;
	}

	while (remaining > 0U) {
		const size_t first =
			MIN(remaining, pw_state.buffer_size - pw_state.write_idx);

		memcpy(&pw_state.buffer[pw_state.write_idx], src, first);
		pw_state.write_idx = (pw_state.write_idx + first) % pw_state.buffer_size;
		pw_state.fill_level += first;
		src += first;
		remaining -= first;
	}

	pthread_mutex_unlock(&pw_state.lock);

	return 0;
}
