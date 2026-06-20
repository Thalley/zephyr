/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth BAP Broadcast Sink sample PipeWire header
 *
 * This file handles all the PipeWire related functionality for the sample
 */

#ifndef SAMPLE_BAP_BROADCAST_SINK_PIPEWIRE_H
#define SAMPLE_BAP_BROADCAST_SINK_PIPEWIRE_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>

/** Sample rate used for PipeWire playback (Hz) */
#define PIPEWIRE_SAMPLE_RATE_HZ 48000U

/**
 * @brief Add a decoded LC3 frame to the PipeWire output buffer
 *
 * @param chan_allocation The channel of the frame (@ref BT_AUDIO_LOCATION_FRONT_LEFT,
 *                        @ref BT_AUDIO_LOCATION_FRONT_RIGHT or
 *                        @ref BT_AUDIO_LOCATION_MONO_AUDIO)
 * @param frame           Pointer to the decoded PCM frame
 * @param frame_size      Size of @p frame in octets
 * @param ts              Timestamp of the frame
 *
 * @retval 0        Success
 * @retval -EINVAL  Invalid channel, frame or frame size
 * @retval -ENOEXEC Old timestamp; frame discarded
 * @retval -ENOMEM  No memory to enqueue the frame
 */
int pipewire_add_frame_to_pipewire(enum bt_audio_location chan_allocation, const int16_t *frame,
				   size_t frame_size, uint32_t ts);

/**
 * @brief Clear the last accumulated SDU
 *
 * Call this when only part of an SDU could be decoded.
 */
void pipewire_clear_frames_to_pipewire(void);

/**
 * @brief Initialize the PipeWire output module
 *
 * @retval 0    Success
 * @retval -EIO Failed to initialize PipeWire
 */
int pipewire_init(void);

#endif /* SAMPLE_BAP_BROADCAST_SINK_PIPEWIRE_H */
