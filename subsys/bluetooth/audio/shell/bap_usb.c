/**
 * @file
 * @brief Bluetooth Basic Audio Profile shell USB extension
 *
 * This files handles all the USB related functionality to audio in/out for the BAP shell
 *
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/assigned_numbers.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>

#if defined(CONFIG_SOC_NRF5340_CPUAPP)
#if defined(CONFIG_CLOCK_CONTROL_NRF)
#include <nrfx_clock.h>
#else
#include <nrfx_clock_hfclk.h>
#endif
#include <drivers/nrfx_errors.h>
#include <hal/nrf_clock.h>
#endif /* CONFIG_SOC_NRF5340_CPUAPP */

#include "audio.h"

LOG_MODULE_REGISTER(bap_usb, CONFIG_BT_BAP_STREAM_LOG_LEVEL);

#define USB_LOG_RATE           (30U * MSEC_PER_SEC) /* 30 seconds */
#define USB_FRAME_DURATION_US  1000U
#define USB_SAMPLE_CNT         ((USB_FRAME_DURATION_US * USB_SAMPLE_RATE) / USEC_PER_SEC)
#define USB_BYTES_PER_SAMPLE   sizeof(int16_t)
#define USB_MONO_FRAME_SIZE    (USB_SAMPLE_CNT * USB_BYTES_PER_SAMPLE)
#define USB_STEREO_FRAME_SIZE  (USB_MONO_FRAME_SIZE * USB_CHANNELS)

/* Both ring buffers hold interleaved stereo data, and all cursors into them are in USB frames
 * (left+right sample pairs) rather than in samples or octets.
 *
 * liblc3 is set up with an output sample rate of USB_SAMPLE_RATE for every stream (see
 * init_lc3_encoder() and init_lc3_decoder()), so a stream always produces and consumes PCM at
 * USB_SAMPLE_RATE regardless of its configured LC3 sample rate. Streams thus only differ in how
 * many USB frames a single LC3 frame covers: 120, 240, 360 or 480 for 2.5ms, 5ms, 7.5ms and 10ms
 * frame durations respectively. USB itself transfers USB_SAMPLE_CNT (48) frames every SOF.
 *
 * By sizing the ring buffers to a multiple of the least common multiple of all of those
 * (LCM(48, 120, 240, 360, 480) == 1440), and by keeping every cursor a multiple of its own step
 * size, neither an LC3 frame nor a USB transfer can ever straddle the end of a ring buffer. That
 * removes the need for any wrap handling, and lets both liblc3 and the USB DMA operate directly
 * on the ring buffers.
 *
 * Note that the 44.1kHz LC3 configurations have non-integer frame durations (8.16ms and 10.88ms)
 * which would break this. They cannot reach this code as stream_started_cb() rejects any
 * frequency that is not 8, 16, 24, 32 or 48kHz, but supporting them would require revisiting the
 * ring buffer sizing.
 */
#define USB_RING_ALIGN_FRAMES 1440U /* LCM(48, 120, 240, 360, 480) == 30ms */
#define USB_RING_FRAMES       (USB_RING_ALIGN_FRAMES * 2U) /* 60ms */
#define USB_RING_SAMPLES      (USB_RING_FRAMES * USB_CHANNELS)

/* Cursors count frames monotonically rather than modulo USB_RING_FRAMES, and are only reduced to
 * a ring buffer index when the buffer is actually addressed. If they wrapped with the ring, a
 * consumer that did not sample often enough could miss the producer lapping it entirely: the
 * distance would pass through the detection band and come back looking small. With a modulus
 * this much larger than the ring, a lap is detectable for hours rather than for the ~10ms it
 * takes the producer to cross the ring.
 *
 * The modulus is a multiple of USB_RING_FRAMES so that reducing a cursor to an index is
 * continuous across the wrap, and it stays below INT32_MAX so that it fits an atomic_t.
 */
#define USB_CURSOR_MODULUS (USB_RING_FRAMES * 524288U)

/* Maximum number of USB frames covered by a single LC3 frame */
#define USB_MAX_FRAMES_PER_LC3_FRAME                                                               \
	((LC3_MAX_FRAME_DURATION_US * USB_SAMPLE_RATE) / USEC_PER_SEC)

BUILD_ASSERT((USB_RING_FRAMES % USB_RING_ALIGN_FRAMES) == 0U,
	     "The ring buffers must be a multiple of USB_RING_ALIGN_FRAMES");
BUILD_ASSERT((USB_RING_ALIGN_FRAMES % USB_SAMPLE_CNT) == 0U,
	     "A USB transfer must never straddle the end of a ring buffer");
BUILD_ASSERT((USB_RING_ALIGN_FRAMES % USB_MAX_FRAMES_PER_LC3_FRAME) == 0U,
	     "An LC3 frame must never straddle the end of a ring buffer");
BUILD_ASSERT((USB_STEREO_FRAME_SIZE % USB_BUF_GRANULARITY) == 0U,
	     "USB transfers out of the ring buffers must be a multiple of the DMA granularity");
BUILD_ASSERT((USB_CURSOR_MODULUS % USB_RING_FRAMES) == 0U,
	     "Reducing a cursor to a ring buffer index must be continuous across the wrap");
BUILD_ASSERT(USB_CURSOR_MODULUS <= (size_t)INT32_MAX, "A cursor must fit an atomic_t");
BUILD_ASSERT(USB_CURSOR_MODULUS > (USB_RING_FRAMES * 2U),
	     "The cursor modulus must be large enough to make a lap detectable");

#define IN_TERMINAL_ID  UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))
#define OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))

#if defined(CONFIG_BT_AUDIO_RX)
static void usb_data_request(const struct device *dev);
#endif /* CONFIG_BT_AUDIO_RX */

#if defined(CONFIG_BT_AUDIO_TX)
static void usb_out_terminal_disabled(void);
#endif /* CONFIG_BT_AUDIO_TX */

size_t bap_usb_get_read_cnt(const struct shell_stream *sh_stream)
{
	return (USB_SAMPLE_CNT * sh_stream->lc3_frame_duration_us) / USEC_PER_MSEC;
}

/** Reduce a monotonic cursor to an index into the ring buffers, in frames */
static size_t usb_ring_index(size_t cursor)
{
	__ASSERT(cursor < USB_CURSOR_MODULUS, "Invalid cursor %zu", cursor);

	return cursor % USB_RING_FRAMES;
}

/**
 * Round @p frames down to a multiple of @p step, so that a cursor that is snapped to another
 * cursor keeps the alignment that the ring buffer sizing relies on.
 */
static size_t usb_align_down(size_t frames, size_t step)
{
	__ASSERT(step != 0U, "Invalid step");
	__ASSERT((USB_RING_FRAMES % step) == 0U, "Step %zu does not divide the ring buffer", step);

	return frames - (frames % step);
}

/** Number of frames produced between @p from and @p to */
static size_t usb_frames_between(size_t from, size_t to)
{
	if (to >= from) {
		return to - from;
	}

	return to + (USB_CURSOR_MODULUS - from);
}

/** Advance @p cursor by @p frames */
static size_t usb_advance(size_t cursor, size_t frames)
{
	cursor += frames;

	if (cursor >= USB_CURSOR_MODULUS) {
		cursor -= USB_CURSOR_MODULUS;
	}

	__ASSERT(cursor < USB_CURSOR_MODULUS, "Invalid cursor %zu", cursor);

	return cursor;
}

static bool in_terminal_enabled;
static bool out_terminal_enabled;
static void usb_terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
				   bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	if (terminal == IN_TERMINAL_ID) {
		in_terminal_enabled = enabled;
	} else if (terminal == OUT_TERMINAL_ID) {
		out_terminal_enabled = enabled;
#if defined(CONFIG_BT_AUDIO_TX)
		if (!enabled) {
			usb_out_terminal_disabled();
		}
#endif /* CONFIG_BT_AUDIO_TX */
	} else {
		/* no-op */
	}
}

static void usb_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

#if defined CONFIG_BT_AUDIO_RX
	if (in_terminal_enabled) {
		usb_data_request(dev);
	} /* else no-op, but is mandatory to register */
#endif /* CONFIG_BT_AUDIO_RX */
}

#if defined CONFIG_BT_AUDIO_RX
/* Interleaved stereo ring buffer holding decoded audio on its way to the USB host.
 *
 * It has up to 2 producers (a single stream for each of the left and right channels, elected by
 * stream_started_cb()) that each have their own write cursor, and a single consumer (the USB SOF
 * handler). The buffer is written directly by liblc3 and read directly by the USB DMA.
 */
USB_STATIC_BUF_DEFINE(usb_in_ring_buf_mem, USB_RING_SAMPLES * USB_BYTES_PER_SAMPLE);
static int16_t *const usb_in_ring_buf = (int16_t *)usb_in_ring_buf_mem;
/* Sent when there is nothing to send. Kept separate from the ring buffer so that an underrun does
 * not discard data that a channel has already decoded.
 */
USB_STATIC_BUF_DEFINE(usb_in_silence_mem, USB_STEREO_FRAME_SIZE);
static int16_t *const usb_in_silence = (int16_t *)usb_in_silence_mem;
/* Written by the decoder thread, read by the USB SOF handler */
static atomic_t usb_in_left_write_cursor;
static atomic_t usb_in_right_write_cursor;
/* Written by the USB SOF handler, read by the decoder thread */
static atomic_t usb_in_read_cursor;
/* Written when a stream starts or stops, read by the USB SOF handler */
static atomic_t usb_in_left_active;
static atomic_t usb_in_right_active;
/* Set when a channel is activated, cleared by the decoder thread once it has placed the write
 * cursor of that channel, which it can only do when it knows the frame size of the stream
 */
static atomic_t usb_in_left_needs_resync;
static atomic_t usb_in_right_needs_resync;
/* Number of consecutive underruns, used to bound how long a single starving channel may hold
 * back a channel that is still producing data. Only accessed by the USB SOF handler.
 */
static size_t usb_in_underrun_cnt;

/* Amount of data to keep between the read cursor and a write cursor, to absorb the jitter of the
 * incoming SDUs. Also used as the target when a channel has to be resynchronized.
 */
#define USB_IN_TARGET_PREFILL_FRAMES (USB_MAX_FRAMES_PER_LC3_FRAME * 2U) /* 20ms */

/* Number of consecutive underruns after which a starving channel is sent as silence rather than
 * blocking the channels that do produce data. A stream that stops without being deactivated would
 * otherwise mute the other channel indefinitely.
 */
#define USB_IN_MAX_UNDERRUNS 100U /* 100ms */

/* Each of the cursors above has a single writer, so they are accessed with atomics rather than a
 * lock. This keeps the USB SOF handler free of both blocking and priority inversion.
 */

/** Number of frames that @p write_cursor is ahead of @p read_cursor */
static size_t usb_in_chan_fill(size_t read_cursor, size_t write_cursor)
{
	return usb_frames_between(read_cursor, write_cursor);
}

/* USB consumer callback, called every 1ms, consumes USB_SAMPLE_CNT frames from the ring buffer */
static void usb_data_request(const struct device *dev)
{
	const size_t read_cursor = (size_t)atomic_get(&usb_in_read_cursor);
	bool left_active = atomic_get(&usb_in_left_active) == 1;
	bool right_active = atomic_get(&usb_in_right_active) == 1;
	bool starving = false;
	int16_t *pcm_buf;
	bool give_up;
	bool have_data;
	int err;

	/* A channel that has starved for too long is ignored entirely, so that a channel which
	 * does produce data is not held back indefinitely. This bounds the effect of a producer
	 * that stops without deactivating its channel.
	 */
	give_up = usb_in_underrun_cnt >= USB_IN_MAX_UNDERRUNS;

	/* A channel that has been activated but has not yet placed its first frame has a stale
	 * write cursor, which could otherwise look like a full ring of valid data
	 */
	if (left_active &&
	    (atomic_get(&usb_in_left_needs_resync) == 1 ||
	     usb_in_chan_fill(read_cursor, (size_t)atomic_get(&usb_in_left_write_cursor)) <
		     USB_SAMPLE_CNT)) {
		left_active = !give_up;
		starving = true;
	}

	if (right_active &&
	    (atomic_get(&usb_in_right_needs_resync) == 1 ||
	     usb_in_chan_fill(read_cursor, (size_t)atomic_get(&usb_in_right_write_cursor)) <
		     USB_SAMPLE_CNT)) {
		right_active = !give_up;
		starving = true;
	}

	/* Only consume data that every channel that is still considered active has produced */
	have_data = (left_active || right_active) && (!starving || give_up);

	/* Acquire the PCM that the decoder thread published before it advanced the write cursors
	 * that were read above
	 */
	barrier_dmem_fence_full();

	if (have_data) {
		static size_t cnt;

		pcm_buf = &usb_in_ring_buf[usb_ring_index(read_cursor) * USB_CHANNELS];

		if (left_active != right_active) {
			/* Duplicate the single active channel to both. This is the only per-sample
			 * operation left on this path.
			 */
			const size_t src = left_active ? 0U : 1U;
			const size_t dst = left_active ? 1U : 0U;

			for (size_t i = 0U; i < USB_SAMPLE_CNT; i++) {
				pcm_buf[(i * USB_CHANNELS) + dst] =
					pcm_buf[(i * USB_CHANNELS) + src];
			}
		}

		if (!starving) {
			usb_in_underrun_cnt = 0U;
		}

		cnt++;
		LOG_DBG_RATELIMIT_RATE(USB_LOG_RATE, "[%zu]: Sending USB audio", cnt);
	} else {
		static size_t cnt;

		/* Underrun. Send silence and leave the read cursor alone, so that the data that is
		 * still being decoded is not skipped.
		 */
		pcm_buf = usb_in_silence;
		(void)memset(pcm_buf, 0, USB_STEREO_FRAME_SIZE);

		if (usb_in_underrun_cnt < USB_IN_MAX_UNDERRUNS) {
			usb_in_underrun_cnt++;
		}

		cnt++;
		LOG_WRN_RATELIMIT_RATE(USB_LOG_RATE, "[%zu]: Sending silent USB audio", cnt);
	}

	err = usbd_uac2_send(dev, IN_TERMINAL_ID, pcm_buf, USB_STEREO_FRAME_SIZE);
	if (err != 0) {
		static size_t cnt;

		cnt++;
		LOG_ERR_RATELIMIT_RATE(USB_LOG_RATE, "Failed to send USB audio: %d (%zu)", err,
				       cnt);

		return;
	}

	if (have_data) {
		/* Release the frames only once they have been handed to the USB stack, so that a
		 * producer cannot overwrite them while they are still in flight
		 */
		atomic_set(&usb_in_read_cursor,
			   (atomic_val_t)usb_advance(read_cursor, USB_SAMPLE_CNT));
	}
}

static void usb_buf_release_cb(const struct device *dev, uint8_t terminal, void *buf,
			       void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(buf);
	ARG_UNUSED(user_data);

	/* The buffer is part of usb_in_ring_buf and is not owned by the USB stack */
}

static atomic_t *usb_in_write_cursor(enum bt_audio_location chan_alloc)
{
	if (chan_alloc == BT_AUDIO_LOCATION_FRONT_RIGHT) {
		return &usb_in_right_write_cursor;
	}

	/* Mono is stored in, and sent from, the left channel */
	return &usb_in_left_write_cursor;
}

static atomic_t *usb_in_needs_resync(enum bt_audio_location chan_alloc)
{
	if (chan_alloc == BT_AUDIO_LOCATION_FRONT_RIGHT) {
		return &usb_in_right_needs_resync;
	}

	return &usb_in_left_needs_resync;
}

static size_t usb_in_chan_offset(enum bt_audio_location chan_alloc)
{
	return chan_alloc == BT_AUDIO_LOCATION_FRONT_RIGHT ? 1U : 0U;
}

void bap_usb_activate_in_chan(enum bt_audio_location chan_alloc)
{
	const bool is_right = chan_alloc == BT_AUDIO_LOCATION_FRONT_RIGHT;

	/* The write cursor is owned by the decoder thread, so it is not placed here. The channel
	 * is instead marked for resynchronization, which the decoder thread performs on its first
	 * frame, when the frame size of the stream is known.
	 */
	atomic_set(usb_in_needs_resync(chan_alloc), 1);
	atomic_set(is_right ? &usb_in_right_active : &usb_in_left_active, 1);

	LOG_INF("Activated USB IN channel 0x%08X", (uint32_t)chan_alloc);
}

void bap_usb_deactivate_in_chan(enum bt_audio_location chan_alloc)
{
	const bool is_right = chan_alloc == BT_AUDIO_LOCATION_FRONT_RIGHT;

	atomic_set(is_right ? &usb_in_right_active : &usb_in_left_active, 0);

	LOG_INF("Deactivated USB IN channel 0x%08X", (uint32_t)chan_alloc);
}

/**
 * Place a channel at USB_IN_TARGET_PREFILL_FRAMES ahead of the consumer, and fill the frames it
 * skips over with silence so that the consumer never sends stale ring buffer content.
 *
 * The cursor is rounded up to a multiple of @p sample_cnt so that it stays at or ahead of the
 * consumer, and so that the LC3 frames written from it never straddle the end of the ring buffer.
 *
 * Some of the silence is written to frames that the consumer may be sending concurrently. That is
 * harmless, as this channel has no valid data for those frames either way, and the alternative is
 * to send stale ring buffer content.
 */
static size_t usb_in_resync(enum bt_audio_location chan_alloc, size_t sample_cnt)
{
	const size_t chan_offset = usb_in_chan_offset(chan_alloc);
	const size_t read_cursor = (size_t)atomic_get(&usb_in_read_cursor);
	size_t cursor = usb_align_down(read_cursor, sample_cnt);
	size_t silence_cnt;

	if (cursor != read_cursor) {
		cursor = usb_advance(cursor, sample_cnt);
	}

	cursor = usb_advance(cursor, usb_align_down(USB_IN_TARGET_PREFILL_FRAMES, sample_cnt));

	silence_cnt = usb_frames_between(read_cursor, cursor);
	for (size_t i = 0U; i < silence_cnt; i++) {
		const size_t frame = usb_advance(read_cursor, i);

		usb_in_ring_buf[(usb_ring_index(frame) * USB_CHANNELS) + chan_offset] = 0;
	}

	return cursor;
}

int16_t *bap_usb_claim_in_frame(enum bt_audio_location chan_alloc, size_t sample_cnt)
{
	atomic_t *cursor_ptr = usb_in_write_cursor(chan_alloc);
	size_t cursor;
	size_t fill;

	if (sample_cnt == 0U || sample_cnt > USB_MAX_FRAMES_PER_LC3_FRAME ||
	    (USB_RING_FRAMES % sample_cnt) != 0U) {
		LOG_WRN_RATELIMIT("Invalid sample count %zu", sample_cnt);

		return NULL;
	}

	cursor = (size_t)atomic_get(cursor_ptr);
	fill = usb_in_chan_fill((size_t)atomic_get(&usb_in_read_cursor), cursor);

	/* Resynchronize the channel if it has just been activated, if it has fallen behind the
	 * consumer (which has already sent silence for the data being decoded now), if it has run
	 * so far ahead that it is about to overwrite data that has not been sent yet, or if it is
	 * not aligned to its own frame size.
	 */
	if (atomic_get(usb_in_needs_resync(chan_alloc)) == 1 || fill < sample_cnt ||
	    fill > (USB_RING_FRAMES - USB_MAX_FRAMES_PER_LC3_FRAME) ||
	    (cursor % sample_cnt) != 0U) {
		cursor = usb_in_resync(chan_alloc, sample_cnt);
		atomic_set(cursor_ptr, (atomic_val_t)cursor);

		/* Cleared last, so that the consumer never sees a cleared flag together with the
		 * stale write cursor that the flag exists to hide
		 */
		atomic_set(usb_in_needs_resync(chan_alloc), 0);

		LOG_WRN_RATELIMIT_RATE(USB_LOG_RATE,
				       "Resynchronized USB IN channel 0x%08X (fill was %zu)",
				       (uint32_t)chan_alloc, fill);
	}

	return &usb_in_ring_buf[(usb_ring_index(cursor) * USB_CHANNELS) +
				usb_in_chan_offset(chan_alloc)];
}

void bap_usb_release_in_frame(enum bt_audio_location chan_alloc, size_t sample_cnt)
{
	atomic_t *cursor_ptr = usb_in_write_cursor(chan_alloc);
	static size_t cnt;

	/* Publish the decoded PCM before the consumer can observe the advanced cursor */
	barrier_dmem_fence_full();
	atomic_set(cursor_ptr,
		   (atomic_val_t)usb_advance((size_t)atomic_get(cursor_ptr), sample_cnt));

	cnt++;
	LOG_DBG_RATELIMIT_RATE(USB_LOG_RATE, "[%zu]: Added USB audio frame", cnt);
}
#endif /* CONFIG_BT_AUDIO_RX */

#if defined(CONFIG_BT_AUDIO_TX)
/* Interleaved stereo ring buffer holding audio received from the USB host.
 *
 * It has a single producer (the USB OUT endpoint, which writes into it by DMA) and 0 or more
 * consumers, one per TX stream, that each have their own read cursor and that may consume at
 * different rates. The buffer is read directly by liblc3.
 */
USB_STATIC_BUF_DEFINE(usb_out_ring_buf_mem, USB_RING_SAMPLES * USB_BYTES_PER_SAMPLE);
static int16_t *const usb_out_ring_buf = (int16_t *)usb_out_ring_buf_mem;
/* Points to the oldest/uninitialized data. Written by the USB OUT callbacks, read by the encoder
 * thread.
 */
static atomic_t usb_out_write_cursor;
/* Position that has been handed to the USB stack but not yet received. The UAC2 class may have
 * up to 2 transfers queued at a time, so this may be ahead of usb_out_write_cursor.
 */
static size_t usb_out_pending_cursor;
/* Number of frames in a single USB transfer. This is the wMaxPacketSize of the OUT endpoint, and
 * is thus USB_SAMPLE_CNT when operating at full speed, but only an eighth of that at high speed
 * where a transfer covers a microframe rather than a frame.
 */
static size_t usb_out_slot_frames = USB_SAMPLE_CNT;

/* Amount of data a stream aims to keep between itself and the write cursor. Used when a stream
 * starts, when it underruns, and when it has to be resynchronized because it was about to be
 * overwritten.
 *
 * This must exceed the amount of PCM that the encoder can consume back to back, which is one SDU
 * per outstanding send credit. The credits are primed to PRIME_COUNT, so a cushion of exactly two
 * SDUs can be emptied by a single burst, leaving the next SDU to underrun.
 */
#define USB_OUT_TARGET_PREFILL_FRAMES (USB_MAX_FRAMES_PER_LC3_FRAME * 3U) /* 30ms */

/* Number of consecutive underrunning attempts after which a stream rebuilds its cushion. The
 * encoder thread retries roughly every millisecond, so this allows several USB transfers to land
 * before giving up on the stream catching up on its own.
 */
#define USB_OUT_UNDERRUN_PREFILL_THRESHOLD 5U

/* usb_out_write_cursor is written only by the USB OUT callbacks and usb_out_pending_cursor and
 * usb_out_slot_frames are only ever touched by them, so this path needs no lock either. Each
 * consumer owns its own read cursor and evaluates itself in bap_usb_claim_frame_block(), which
 * means the producer never has to walk the streams.
 */

static void usb_out_terminal_disabled(void)
{
	/* Any queued transfers are discarded by the USB stack, so the slots that were handed out
	 * become available again
	 */
	usb_out_pending_cursor = (size_t)atomic_get(&usb_out_write_cursor);

	/* The write cursor is only aligned to the slot size that was in use, so invalidate the
	 * slot size to force usb_get_recv_buf_cb() to realign when the terminal is enabled again
	 */
	usb_out_slot_frames = 0U;
}

/** Move @p cursor @p frames backwards, wrapping around the start of the ring buffer */
static size_t usb_retreat(size_t cursor, size_t frames)
{
	__ASSERT(frames <= USB_RING_FRAMES, "Invalid frame count %zu", frames);
	__ASSERT(cursor < USB_CURSOR_MODULUS, "Invalid cursor %zu", cursor);

	if (cursor >= frames) {
		return cursor - frames;
	}

	return cursor + (USB_CURSOR_MODULUS - frames);
}

/** Number of frames that @p sh_stream can still read before catching up with the producer */
static size_t usb_out_stream_avail(const struct shell_stream *sh_stream)
{
	return usb_frames_between(sh_stream->tx.usb_read_cursor,
				  (size_t)atomic_get(&usb_out_write_cursor));
}

/**
 * Move @p sh_stream forwards to the most recent data if the producer has lapped it, and report
 * how many frames it may read.
 *
 * This is evaluated by the stream itself, rather than by the producer for every stream on every
 * USB transfer, so that the cost is proportional to the number of streams actually reading and
 * so that the producer never touches consumer state.
 */
static size_t usb_out_stream_sync(struct shell_stream *sh_stream, size_t read_cnt)
{
	size_t avail = usb_out_stream_avail(sh_stream);

	/* Each stream is evaluated against its own margin, as they may consume at different
	 * rates, and a stream that is not reading at all must not hold back the producer.
	 */
	if (avail > (USB_RING_FRAMES - USB_MAX_FRAMES_PER_LC3_FRAME)) {
		/* Drop the backlog and keep the most recent data, so that the stream does not
		 * immediately fall behind again
		 */
		sh_stream->tx.usb_read_cursor = usb_retreat(
			usb_align_down((size_t)atomic_get(&usb_out_write_cursor), read_cnt),
			usb_align_down(USB_OUT_TARGET_PREFILL_FRAMES, read_cnt));

		avail = usb_out_stream_avail(sh_stream);

		LOG_WRN_RATELIMIT_RATE(USB_LOG_RATE,
				       "Resynchronized USB OUT stream %p (avail was %zu)",
				       (void *)sh_stream, avail);
	}

	return avail;
}

void bap_usb_tx_stream_started(struct shell_stream *sh_stream)
{
	const size_t read_cnt = bap_usb_get_read_cnt(sh_stream);

	if (read_cnt == 0U) {
		LOG_WRN("Invalid frame duration %u for stream %p",
			sh_stream->lc3_frame_duration_us, (void *)sh_stream);
		return;
	}

	__ASSERT((USB_RING_FRAMES % read_cnt) == 0U,
		 "Read count %zu does not divide the ring buffer", read_cnt);

	/* Start at the producer so that only data received after the stream started is sent,
	 * rather than at whatever the union with the RX state left in the field.
	 */
	sh_stream->tx.usb_read_cursor =
		usb_align_down((size_t)atomic_get(&usb_out_write_cursor), read_cnt);
	sh_stream->tx.usb_needs_prefill = true;
	sh_stream->tx.usb_underrun_cnt = 0U;
}

static void *usb_get_recv_buf_cb(const struct device *dev, uint8_t terminal, uint16_t size,
				 void *user_data)
{
	size_t frame_cnt;
	void *buf;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (!out_terminal_enabled) {
		return NULL;
	}

	/* The USB DMA may write up to size octets, so the slot handed out below must be that
	 * large. size is the wMaxPacketSize of the endpoint and thus constant while the terminal
	 * is enabled, which lets usb_data_recv_cb() commit the same amount.
	 */
	frame_cnt = size / (USB_CHANNELS * USB_BYTES_PER_SAMPLE);
	if (frame_cnt == 0U || frame_cnt > USB_SAMPLE_CNT ||
	    (size % (USB_CHANNELS * USB_BYTES_PER_SAMPLE)) != 0U ||
	    (USB_RING_FRAMES % frame_cnt) != 0U) {
		LOG_WRN_RATELIMIT("Unsupported receive buffer size %u", size);

		return NULL;
	}

	if (frame_cnt != usb_out_slot_frames) {
		/* Keep the cursors aligned to the new transfer size */
		usb_out_slot_frames = frame_cnt;
		usb_out_pending_cursor =
			usb_align_down((size_t)atomic_get(&usb_out_write_cursor), frame_cnt);
		atomic_set(&usb_out_write_cursor, (atomic_val_t)usb_out_pending_cursor);
	}

	/* Hand out the next unused slot in the ring buffer, so that the USB DMA writes directly
	 * into it. The slot is committed by usb_data_recv_cb().
	 */
	buf = &usb_out_ring_buf[usb_ring_index(usb_out_pending_cursor) * USB_CHANNELS];
	usb_out_pending_cursor = usb_advance(usb_out_pending_cursor, frame_cnt);

	return buf;
}

static void usb_data_recv_cb(const struct device *dev, uint8_t terminal, void *buf, uint16_t size,
			     void *user_data)
{
	static size_t cnt;
	size_t frame_cnt;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (buf == NULL || usb_out_slot_frames == 0U) {
		/* No slot was handed out for this buffer, e.g. because the terminal was disabled
		 * in between, so there is nothing to commit
		 */
		return;
	}

	/* The data has been written into the ring buffer by DMA already, so all that is left is
	 * to make it available to the consumers. The host may send a short packet, in which case
	 * the remainder of the slot is zero-filled; the entire slot is always committed so that
	 * the cursors keep the alignment that the ring buffer sizing relies on.
	 */
	frame_cnt = MIN(size / (USB_CHANNELS * USB_BYTES_PER_SAMPLE), usb_out_slot_frames);
	if (frame_cnt < usb_out_slot_frames) {
		int16_t *pcm = (int16_t *)buf;

		(void)memset(&pcm[frame_cnt * USB_CHANNELS], 0,
			     (usb_out_slot_frames - frame_cnt) * USB_CHANNELS *
				     USB_BYTES_PER_SAMPLE);

		LOG_DBG_RATELIMIT_RATE(USB_LOG_RATE, "Received short USB packet of %u octets",
				       size);
	}

	/* Publish the received PCM before the consumers can observe the advanced cursor. Streams
	 * that have fallen too far behind resynchronize themselves when they next read, so no
	 * stream state is touched here.
	 */
	barrier_dmem_fence_full();
	atomic_set(&usb_out_write_cursor,
		   (atomic_val_t)usb_advance((size_t)atomic_get(&usb_out_write_cursor),
					     usb_out_slot_frames));

	cnt++;
	LOG_DBG_RATELIMIT_RATE(USB_LOG_RATE, "USB Data received (count = %zu)", cnt);
}

bool bap_usb_can_get_full_sdu(struct shell_stream *sh_stream)
{
	const size_t read_cnt = bap_usb_get_read_cnt(sh_stream);
	const size_t retrieve_cnt = read_cnt * sh_stream->lc3_frame_blocks_per_sdu;
	size_t avail;

	if (read_cnt == 0U || retrieve_cnt == 0U) {
		return false;
	}

	/* Resynchronize the stream if the producer has lapped it while it was not reading */
	avail = usb_out_stream_sync(sh_stream, read_cnt);

	/* Acquire the PCM that the producer published before it advanced its cursor */
	barrier_dmem_fence_full();

	/* Build up a cushion before sending, at the cost of presentation delay. This is tracked
	 * per stream, as streams may start at different times and consume at different rates. At
	 * least two SDUs are always required, so that a stream whose SDUs are longer than the
	 * target still gets a whole spare SDU.
	 */
	if (sh_stream->tx.usb_needs_prefill) {
		const size_t prefill_cnt = MAX(retrieve_cnt * 2U,
					       usb_align_down(USB_OUT_TARGET_PREFILL_FRAMES,
							      read_cnt));

		if (avail < prefill_cnt) {
			return false;
		}

		sh_stream->tx.usb_needs_prefill = false;
	}

	if (avail < retrieve_cnt) {
		/* Not enough for a frame yet */
		if (sh_stream->tx.usb_underrun_cnt == 0U) {
			LOG_WRN_RATELIMIT("Ring buffer (%zu/%u) does not contain enough for an "
					  "entire SDU %zu for channel allocation 0x%08X",
					  avail, USB_RING_FRAMES, retrieve_cnt,
					  (uint32_t)sh_stream->lc3_chan_allocation);
		}

		if (sh_stream->tx.usb_underrun_cnt < UINT16_MAX) {
			sh_stream->tx.usb_underrun_cnt++;
		}

		/* The caller retries rather than sending an empty SDU, so a brief shortfall
		 * resolves itself as soon as the next USB transfer lands. Retrying for this long
		 * without succeeding means the stream is persistently starved rather than merely
		 * late, so rebuild the cushion instead of continuing to ride the edge.
		 */
		if (sh_stream->tx.usb_underrun_cnt == USB_OUT_UNDERRUN_PREFILL_THRESHOLD) {
			sh_stream->tx.usb_needs_prefill = true;
		}

		return false;
	}

	sh_stream->tx.usb_underrun_cnt = 0U;

	return true;
}

const int16_t *bap_usb_claim_frame_block(struct shell_stream *sh_stream)
{
	const size_t read_cnt = bap_usb_get_read_cnt(sh_stream);

	if (read_cnt == 0U) {
		return NULL;
	}

	__ASSERT((sh_stream->tx.usb_read_cursor % read_cnt) == 0U,
		 "Misaligned cursor %zu for read count %zu", sh_stream->tx.usb_read_cursor,
		 read_cnt);
	__ASSERT(sh_stream->tx.usb_read_cursor < USB_CURSOR_MODULUS, "Invalid cursor %zu",
		 sh_stream->tx.usb_read_cursor);

	/* The ring buffer size is a multiple of read_cnt, so the frame block never straddles the
	 * end of the ring buffer and can be handed to liblc3 as-is.
	 */
	return &usb_out_ring_buf[usb_ring_index(sh_stream->tx.usb_read_cursor) * USB_CHANNELS];
}

void bap_usb_release_frame_block(struct shell_stream *sh_stream)
{
	const size_t read_cnt = bap_usb_get_read_cnt(sh_stream);

	if (read_cnt == 0U) {
		return;
	}

	sh_stream->tx.usb_read_cursor = usb_advance(sh_stream->tx.usb_read_cursor, read_cnt);
}
#endif /* CONFIG_BT_AUDIO_TX */

static int bap_usbd_setup_device(struct usbd_context *const bap_usbd)
{
	static const uint8_t attributes =
		(IS_ENABLED(CONFIG_BT_BAP_SHELL_USB_SELF_POWERED) ? USB_SCD_SELF_POWERED : 0U) |
		(IS_ENABLED(CONFIG_BT_BAP_SHELL_USB_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0U);
	USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
	USBD_CONFIGURATION_DEFINE(bap_usb_fs_config, attributes, CONFIG_BT_BAP_SHELL_USB_MAX_POWER,
				  &fs_cfg_desc);
	USBD_DESC_PRODUCT_DEFINE(bap_usb_product, CONFIG_BT_BAP_SHELL_USB_PRODUCT);
	USBD_DESC_MANUFACTURER_DEFINE(bap_usb_mfr, "Zephyr Project");
	USBD_DESC_LANG_DEFINE(bap_usb_lang);
	const uint8_t class_cfg = 0x01U;
	const uint8_t subclass = 0x02U;
	const uint8_t protocol = 0x01U;

	int err;

	err = usbd_add_descriptor(bap_usbd, &bap_usb_lang);
	if (err != 0) {
		LOG_ERR("Failed to initialize language descriptor: %d", err);

		return err;
	}

	err = usbd_add_descriptor(bap_usbd, &bap_usb_mfr);
	if (err != 0) {
		LOG_ERR("Failed to initialize manufacturer descriptor: %d", err);

		return err;
	}

	err = usbd_add_descriptor(bap_usbd, &bap_usb_product);
	if (err != 0) {
		LOG_ERR("Failed to initialize product descriptor: %d", err);

		return err;
	}

	if (IS_ENABLED(CONFIG_HWINFO)) {
		USBD_DESC_SERIAL_NUMBER_DEFINE(bap_usb_sn);

		err = usbd_add_descriptor(bap_usbd, &bap_usb_sn);
		if (err != 0) {
			LOG_ERR("Failed to initialize serial number descriptor: %d", err);

			return err;
		}
	}

	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(bap_usbd) == USBD_SPEED_HS) {
		USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");
		USBD_CONFIGURATION_DEFINE(bap_usb_hs_config, attributes,
					  CONFIG_BT_BAP_SHELL_USB_MAX_POWER, &hs_cfg_desc);

		LOG_DBG("Setting up High-Speed USB");

		err = usbd_add_configuration(bap_usbd, USBD_SPEED_HS, &bap_usb_hs_config);
		if (err != 0) {
			LOG_ERR("Failed to add High-Speed configuration: %d", err);

			return err;
		}

		err = usbd_register_all_classes(bap_usbd, USBD_SPEED_HS, class_cfg, NULL);
		if (err != 0) {
			LOG_ERR("Failed to add register High-Speed classes: %d", err);

			return err;
		}

		err = usbd_device_set_code_triple(bap_usbd, USBD_SPEED_HS, USB_BCC_MISCELLANEOUS,
						  subclass, protocol);
		if (err != 0) {
			LOG_ERR("Failed to set High-Speed code triple: %d", err);

			return err;
		}
	}

	LOG_DBG("Setting up Full-Speed USB");

	err = usbd_add_configuration(bap_usbd, USBD_SPEED_FS, &bap_usb_fs_config);
	if (err != 0) {
		LOG_ERR("Failed to add Full-Speed configuration: %d", err);

		return err;
	}

	err = usbd_register_all_classes(bap_usbd, USBD_SPEED_FS, class_cfg, NULL);
	if (err != 0) {
		LOG_ERR("Failed to register Full-Speed classes: %d", err);

		return err;
	}

	err = usbd_device_set_code_triple(bap_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, subclass,
					  protocol);
	if (err != 0) {
		LOG_ERR("Failed to set Full-Speed code triple: %d", err);

		return err;
	}

	usbd_self_powered(bap_usbd, attributes & USB_SCD_SELF_POWERED);

	return 0;
}

int bap_usb_init(void)
{
	USBD_DEVICE_DEFINE(bap_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
			   CONFIG_BT_BAP_SHELL_USB_VID, CONFIG_BT_BAP_SHELL_USB_PID);
	const struct device *uac2_headset = DEVICE_DT_GET(DT_NODELABEL(uac2_headset));
	static struct uac2_ops usb_audio_ops = {
		.terminal_update_cb = usb_terminal_update_cb,
		.sof_cb = usb_sof_cb,
#if defined(CONFIG_BT_AUDIO_TX)
		.get_recv_buf = usb_get_recv_buf_cb,
		.data_recv_cb = usb_data_recv_cb,
#endif /* CONFIG_BT_AUDIO_TX */
#if defined(CONFIG_BT_AUDIO_RX)
		.buf_release_cb = usb_buf_release_cb,
#endif /* CONFIG_BT_AUDIO_RX */
	};
	int err;

	if (!device_is_ready(uac2_headset)) {
		LOG_ERR("Cannot get USB Headset Device");
		return -EIO;
	}

	usbd_uac2_set_ops(uac2_headset, &usb_audio_ops, NULL);

	err = bap_usbd_setup_device(&bap_usbd);
	if (err != 0) {
		LOG_ERR("Failed to setup USB device: %d", err);
		return err;
	}

	err = usbd_init(&bap_usbd);
	if (err != 0) {
		LOG_ERR("Failed to initialize device support: %d", err);
		return err;
	}

	err = usbd_enable(&bap_usbd);
	if (err != 0) {
		LOG_ERR("Failed to enable USBD: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_SOC_NRF5340_CPUAPP)) {
		/* Use this to turn on 128 MHz clock for the nRF5340 cpu_app
		 * This may not be required, but reduces the risk of not decoding fast enough
		 * to keep up with USB
		 */
#if defined(CONFIG_CLOCK_CONTROL_NRF)
		nrfx_err_t nrfx_err;

		nrfx_err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);

		if (nrfx_err != NRFX_SUCCESS) {
			LOG_WRN("Failed to set 128 MHz: 0x%08X", nrfx_err);
		}
#else
		nrfx_clock_hfclk_divider_set(NRF_CLOCK_HFCLK_DIV_1);
#endif
	}

	LOG_INF("USB audio enabled");

	return 0;
}
