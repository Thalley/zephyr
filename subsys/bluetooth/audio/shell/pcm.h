/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_PCM_H_
#define ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_PCM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>

struct bap_pcm_sink_backend {
	const char *name;
	int (*init)(void);
	int (*write)(const int16_t *frame, size_t frame_size);
};

#if defined(CONFIG_BT_BAP_SHELL_PCM)
int bap_pcm_init(void);
int bap_pcm_stream_started(const void *stream, enum bt_audio_location chan_allocation);
void bap_pcm_stream_stopped(const void *stream);
bool bap_pcm_stream_matches(const void *stream, enum bt_audio_location chan_allocation);
int bap_pcm_add_frame(const void *stream, enum bt_audio_location chan_allocation,
		      const int16_t *frame, size_t frame_size, uint32_t ts);
void bap_pcm_clear_frames(void);
#endif /* CONFIG_BT_BAP_SHELL_PCM */

#if defined(CONFIG_BT_BAP_SHELL_PCM_BACKEND_USB)
extern const struct bap_pcm_sink_backend bap_usb_pcm_backend;
#endif /* CONFIG_BT_BAP_SHELL_PCM_BACKEND_USB */

#if defined(CONFIG_BT_BAP_SHELL_PCM_BACKEND_PIPEWIRE)
extern const struct bap_pcm_sink_backend bap_pipewire_pcm_backend;
#endif /* CONFIG_BT_BAP_SHELL_PCM_BACKEND_PIPEWIRE */

#endif /* ZEPHYR_SUBSYS_BLUETOOTH_AUDIO_SHELL_PCM_H_ */
