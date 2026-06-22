/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BAP Broadcast Source PipeWire bottom layer (host side)
 *
 * Opens a PipeWire capture stream at 48 kHz stereo, decimates the captured
 * audio to the requested broadcast sample rate, and stores the result in a
 * per-channel ring buffer that the Zephyr LC3 encoder thread can drain.
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

/* Capture parameters - matches the PipeWire stream format */
#define CAPTURE_RATE_HZ    48000U
#define CAPTURE_CHANNELS   2U
#define CAPTURE_STRIDE     (CAPTURE_CHANNELS * sizeof(int16_t))

/*
 * Host-side ring buffer for each channel (left = 0, right = 1).
 * Holds up to 200 ms of decimated mono PCM at the highest supported rate
 * (48 kHz) so the buffer is always large enough regardless of the chosen
 * broadcast preset.
 */
#define SOURCE_BUF_MS        200U
#define SOURCE_BUF_SAMPLES   ((SOURCE_BUF_MS * CAPTURE_RATE_HZ) / 1000U)

struct source_ring {
	int16_t *buf;
	size_t capacity; /* in samples */
	size_t read_idx;
	size_t write_idx;
	size_t fill_level;
};

struct pipewire_source_state {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	pthread_mutex_t lock;
	struct source_ring channels[CAPTURE_CHANNELS];
	uint32_t src_rate;
	uint32_t dst_rate;
	bool initialized;
};

static struct pipewire_source_state pw_src_state;

/* Write one mono sample to a channel ring buffer (called with lock held) */
static void ring_put_sample(struct source_ring *ring, int16_t sample)
{
	if (ring->fill_level >= ring->capacity) {
		/* Ring is full – drop the oldest sample */
		ring->read_idx = (ring->read_idx + 1U) % ring->capacity;
		ring->fill_level--;
	}

	ring->buf[ring->write_idx] = sample;
	ring->write_idx = (ring->write_idx + 1U) % ring->capacity;
	ring->fill_level++;
}

/* Read one mono sample from a channel ring buffer (called with lock held).
 * Returns 0 (silence) if the ring is empty.
 */
static int16_t ring_get_sample(struct source_ring *ring)
{
	int16_t sample;

	if (ring->fill_level == 0U) {
		return 0;
	}

	sample = ring->buf[ring->read_idx];
	ring->read_idx = (ring->read_idx + 1U) % ring->capacity;
	ring->fill_level--;

	return sample;
}

static void pipewire_capture_process(void *userdata)
{
	struct pw_buffer *buffer;
	struct spa_buffer *spa_buf;
	struct spa_data *data;
	const int16_t *pcm;
	uint32_t n_frames;
	uint32_t ratio;
	uint32_t i;

	(void)userdata;

	buffer = pw_stream_dequeue_buffer(pw_src_state.stream);
	if (buffer == NULL) {
		return;
	}

	spa_buf = buffer->buffer;
	data = &spa_buf->datas[0];

	if ((data->data == NULL) || (data->chunk->size == 0U)) {
		pw_stream_queue_buffer(pw_src_state.stream, buffer);
		return;
	}

	pcm = (const int16_t *)data->data;
	/* Number of stereo frames in this block */
	n_frames = data->chunk->size / (uint32_t)CAPTURE_STRIDE;

	ratio = pw_src_state.src_rate / pw_src_state.dst_rate;
	if (ratio == 0U) {
		ratio = 1U;
	}

	pthread_mutex_lock(&pw_src_state.lock);

	/* Simple decimation: keep every ratio-th stereo frame */
	for (i = 0U; i < n_frames; i += ratio) {
		const size_t sample_idx = (size_t)i * CAPTURE_CHANNELS;

		ring_put_sample(&pw_src_state.channels[0], pcm[sample_idx]);
		ring_put_sample(&pw_src_state.channels[1], pcm[sample_idx + 1U]);
	}

	pthread_mutex_unlock(&pw_src_state.lock);

	pw_stream_queue_buffer(pw_src_state.stream, buffer);
}

static void pipewire_capture_state_changed(void *userdata, enum pw_stream_state old_state,
					   enum pw_stream_state state, const char *error)
{
	(void)userdata;
	(void)old_state;
	(void)error;

	if ((state == PW_STREAM_STATE_ERROR) || (state == PW_STREAM_STATE_UNCONNECTED)) {
		pw_src_state.initialized = false;
	}
}

static const struct pw_stream_events pipewire_capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pipewire_capture_state_changed,
	.process = pipewire_capture_process,
};

int pipewire_source_bottom_init(uint32_t src_rate, uint32_t dst_rate, const char *target,
				const char *latency)
{
	struct pw_properties *props;
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_S16_LE,
		.channels = CAPTURE_CHANNELS,
		.rate = src_rate,
		.position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR},
	};
	const struct spa_pod *params[1];
	struct spa_pod_builder builder;
	uint8_t pod_buf[256];
	size_t i;
	int err;

	if (pw_src_state.initialized) {
		return 0;
	}

	pw_src_state.src_rate = src_rate;
	pw_src_state.dst_rate = (dst_rate > 0U) ? dst_rate : src_rate;

	for (i = 0U; i < CAPTURE_CHANNELS; i++) {
		pw_src_state.channels[i].buf = calloc(SOURCE_BUF_SAMPLES, sizeof(int16_t));
		if (pw_src_state.channels[i].buf == NULL) {
			err = -ENOMEM;
			goto fail_alloc;
		}

		pw_src_state.channels[i].capacity = SOURCE_BUF_SAMPLES;
	}

	pthread_mutex_init(&pw_src_state.lock, NULL);

	pw_init(NULL, NULL);

	pw_src_state.loop = pw_thread_loop_new("bap-broadcast-source-audio", NULL);
	if (pw_src_state.loop == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
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

	pw_src_state.stream =
		pw_stream_new_simple(pw_thread_loop_get_loop(pw_src_state.loop),
				     "bap-broadcast-source", props,
				     &pipewire_capture_events, NULL);
	if (pw_src_state.stream == NULL) {
		err = -ENOMEM;
		goto fail;
	}

	builder = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

	err = pw_thread_loop_start(pw_src_state.loop);
	if (err < 0) {
		err = -EIO;
		goto fail;
	}

	err = pw_stream_connect(pw_src_state.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
					PW_STREAM_FLAG_RT_PROCESS,
				params, 1U);
	if (err < 0) {
		err = -EIO;
		goto fail;
	}

	pw_src_state.initialized = true;

	return 0;

fail:
	if (pw_src_state.stream != NULL) {
		pw_stream_destroy(pw_src_state.stream);
		pw_src_state.stream = NULL;
	}

	if (pw_src_state.loop != NULL) {
		pw_thread_loop_destroy(pw_src_state.loop);
		pw_src_state.loop = NULL;
	}

	pthread_mutex_destroy(&pw_src_state.lock);

fail_alloc:
	for (i = 0U; i < CAPTURE_CHANNELS; i++) {
		if (pw_src_state.channels[i].buf != NULL) {
			free(pw_src_state.channels[i].buf);
			pw_src_state.channels[i].buf = NULL;
		}
	}

	return err;
}

bool pipewire_source_bottom_get(size_t channel, int16_t *buf, size_t num_samples)
{
	struct source_ring *ring;
	bool full;
	size_t i;

	if (!pw_src_state.initialized || channel >= CAPTURE_CHANNELS || buf == NULL) {
		memset(buf, 0, num_samples * sizeof(int16_t));
		return false;
	}

	ring = &pw_src_state.channels[channel];

	pthread_mutex_lock(&pw_src_state.lock);

	full = (ring->fill_level >= num_samples);

	for (i = 0U; i < num_samples; i++) {
		buf[i] = ring_get_sample(ring);
	}

	pthread_mutex_unlock(&pw_src_state.lock);

	return full;
}
