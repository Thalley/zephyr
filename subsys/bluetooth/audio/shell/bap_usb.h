/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/assigned_numbers.h>

#include "audio.h"

#define BAP_USB_SAMPLE_RATE 48000U

/* The PCM data exchanged with USB is always interleaved stereo at BAP_USB_SAMPLE_RATE. A "USB
 * frame" is a single left+right sample pair, so that all streams advance their cursors by the same
 * amount for the same duration of audio, independently of their channel allocation.
 */
#define BAP_USB_CHANNELS 2U

int bap_usb_init(void);

/* Declared unconditionally as bap.c guards the calls with IS_ENABLED(CONFIG_USBD_AUDIO2_CLASS) */
/** Number of USB frames (left+right sample pairs) covered by a single LC3 frame of @p sh_stream */
size_t bap_usb_get_read_cnt(const struct shell_stream *sh_stream);

/**
 * Place @p sh_stream in the USB OUT ring buffer so that it starts reading data that is old enough
 * to not be overwritten before it is consumed. Shall be called when the stream starts sending.
 */
void bap_usb_tx_stream_started(struct shell_stream *sh_stream);

/**
 * Provide a pointer to @p sh_stream's current position in the interleaved USB OUT ring buffer.
 *
 * The returned pointer is valid for bap_usb_get_read_cnt() interleaved stereo frames
 */
const int16_t *bap_usb_get_frame_block(struct shell_stream *sh_stream);

/**
 * Mark @p chan_alloc as being provided by a stream, so that the USB IN data is taken from the
 * ring buffer rather than being generated from the other channel.
 */
void bap_usb_activate_in_chan(enum bt_audio_location chan_alloc);

/** Mark @p chan_alloc as no longer being provided by any stream */
void bap_usb_deactivate_in_chan(enum bt_audio_location chan_alloc);

/**
 * Provide pointers to the positions in the interleaved USB IN ring buffer that the next decoded
 * frame of each channel in @p chan_allocs shall be written to.
 *
 * @p chan_allocs is @ref BT_AUDIO_LOCATION_MONO_AUDIO, @ref BT_AUDIO_LOCATION_FRONT_LEFT,
 * @ref BT_AUDIO_LOCATION_FRONT_RIGHT or both of the latter for a stereo stream, in which case the
 * channels are kept at the same position. @p frames is indexed by bap_usb_get_chan_offset(), and
 * entries for channels not in @p chan_allocs are set to NULL.
 *
 * @p sample_cnt samples shall be written to each frame with a stride of BAP_USB_CHANNELS, after
 * which the channels shall be advanced together with bap_usb_release_in_frames().
 *
 * @retval 0 Success
 * @retval -EINVAL Invalid @p sample_cnt
 */
int bap_usb_claim_in_frames(enum bt_audio_location chan_allocs, size_t sample_cnt,
			    int16_t *frames[BAP_USB_CHANNELS]);

/** Advance @p chan_allocs past the frames claimed with bap_usb_claim_in_frames() */
void bap_usb_release_in_frames(enum bt_audio_location chan_allocs, size_t sample_cnt);

/** Offset of @p chan_alloc in an interleaved stereo USB frame */
/**
 * @brief Get the offset in an interleaved stereo USB frame from @p chan_alloc
 *
 * @param chan_alloc Either @ref BT_AUDIO_LOCATION_MONO_AUDIO, @ref BT_AUDIO_LOCATION_FRONT_LEFT or
 *                   @ref BT_AUDIO_LOCATION_FRONT_RIGHT
 *
 * @retval 0 For @ref BT_AUDIO_LOCATION_MONO_AUDIO or @ref BT_AUDIO_LOCATION_FRONT_LEFT
 * @retval 1 For @ref BT_AUDIO_LOCATION_FRONT_RIGHT
 */
uint8_t bap_usb_get_chan_offset(enum bt_audio_location chan_alloc);
