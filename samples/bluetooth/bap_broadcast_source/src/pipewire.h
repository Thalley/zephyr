/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth BAP Broadcast Source sample PipeWire header
 *
 * This file handles all the PipeWire related functionality for audio capture
 * in the BAP Broadcast Source sample.
 */

#ifndef SAMPLE_BAP_BROADCAST_SOURCE_PIPEWIRE_H
#define SAMPLE_BAP_BROADCAST_SOURCE_PIPEWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Sample rate at which PipeWire captures audio (Hz) */
#define PIPEWIRE_CAPTURE_RATE_HZ 48000U

/** Number of channels in the PipeWire capture stream */
#define PIPEWIRE_CHANNELS 2U

/**
 * @brief Initialize the PipeWire audio capture module
 *
 * @param dst_sample_rate_hz Target sample rate for the encoder (Hz).
 *                           Captured 48 kHz audio is decimated to this rate.
 *
 * @retval 0    Success
 * @retval -EIO Failed to start the PipeWire capture stream
 */
int pipewire_init(uint32_t dst_sample_rate_hz);

/**
 * @brief Read captured and decimated audio samples for one channel
 *
 * Returns up to @p num_samples mono samples at the rate specified at
 * @ref pipewire_init time.  If fewer samples are available the buffer is
 * zero-padded.
 *
 * @param channel     Channel index: 0 = left, 1 = right
 * @param buf         Destination buffer for int16_t PCM samples
 * @param num_samples Number of samples to read
 *
 * @return true  if @p num_samples were available (no padding needed)
 * @return false if the buffer was partially or fully zero-padded
 */
bool pipewire_get_samples(size_t channel, int16_t *buf, size_t num_samples);

#endif /* SAMPLE_BAP_BROADCAST_SOURCE_PIPEWIRE_H */
