/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bottom layer for the native Bluetooth monitor file backend.
 *
 * This file is compiled in the native_simulator runner context with the host
 * C library.  It therefore uses standard POSIX file I/O rather than Zephyr
 * APIs.
 *
 * Incoming monitor wire-protocol frames are reassembled and written out in
 * btsnoop file format (datalink type 2001 = HCI_MONITOR) so that the
 * resulting file can be opened directly with:
 *
 *   btmon -r /tmp/bt_monitor.log
 */

/* Note: This is used only for interaction with the host C library, and is
 * therefore exempt of coding guidelines rule A.4&5 which applies to the
 * embedded code using embedded libraries.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <nsi_tracing.h>

/* BT monitor wire-protocol constants (kept local; cannot include monitor.h
 * here because that header is compiled in the Zephyr context).
 */
#define MONITOR_BASE_HDR_LEN 6U  /* data_len(2)+opcode(2)+flags(1)+hdr_len(1) */
#define MONITOR_TS32         8U  /* extended-header type for 32-bit timestamp  */

/* btsnoop file format (RFC draft-rfcbt-0 / BlueZ convention)
 *
 * File header (16 bytes):
 *   uint8_t  id[8]      = "btsnoop\0"
 *   uint32_t version    = 1  (big-endian)
 *   uint32_t datalink   = 2001 (HCI_MONITOR, big-endian)
 *
 * Per-packet record (24-byte header + payload):
 *   uint32_t orig_len   (big-endian, == incl_len; we never truncate)
 *   uint32_t incl_len   (big-endian)
 *   uint32_t flags      (big-endian; for HCI_MONITOR: opcode in bits 15:0,
 *                        adapter index in bits 31:16)
 *   uint32_t drops      (big-endian, always 0)
 *   uint64_t ts         (big-endian, microseconds; ts32 * 100 +
 *                        BTSNOOP_EPOCH_OFFSET)
 *   uint8_t  data[incl_len]
 */
#define BTSNOOP_VERSION        1U
#define BTSNOOP_TYPE_MONITOR   2001U
/* Microseconds from Jan 1, 0000 to Jan 1, 1970 (standard btsnoop epoch) */
#define BTSNOOP_EPOCH_OFFSET   UINT64_C(0x00dcddb30f2f8000)

/* Maximum monitor frame we are prepared to buffer.
 * In practice Zephyr BT monitor frames are small (HCI commands/events/ACL
 * PDUs plus a short extended header).  4 KiB covers every realistic case.
 */
#define FRAME_BUF_SIZE 4096U

static FILE   *monitor_file;
static uint8_t frame_buf[FRAME_BUF_SIZE];
static size_t  frame_buf_len;

/* Byte-swap helpers (host is little-endian; btsnoop uses big-endian) */
static uint32_t to_be32(uint32_t v)
{
	return ((v & 0xffU) << 24) | (((v >> 8) & 0xffU) << 16) |
	       (((v >> 16) & 0xffU) << 8) | ((v >> 24) & 0xffU);
}

static uint64_t to_be64(uint64_t v)
{
	return ((uint64_t)to_be32((uint32_t)v) << 32) |
	       (uint64_t)to_be32((uint32_t)(v >> 32));
}

static void write_btsnoop_file_header(void)
{
	static const uint8_t id[8] = { 'b', 't', 's', 'n', 'o', 'o', 'p', 0 };
	uint32_t version_be = to_be32(BTSNOOP_VERSION);
	uint32_t type_be    = to_be32(BTSNOOP_TYPE_MONITOR);

	(void)fwrite(id,          sizeof(id),         1U, monitor_file);
	(void)fwrite(&version_be, sizeof(version_be), 1U, monitor_file);
	(void)fwrite(&type_be,    sizeof(type_be),    1U, monitor_file);
}

static void write_btsnoop_record(uint16_t opcode, uint32_t ts32,
				 const void *data, size_t len)
{
	uint32_t orig_be  = to_be32((uint32_t)len);
	uint32_t flags_be = to_be32((uint32_t)opcode);
	uint32_t zero32   = 0U;
	/* ts32 is in 1/10th-ms units (MONITOR_TS_FREQ = 10000 Hz), so
	 * 1 unit = 100 us.  Add the btsnoop epoch offset to get a valid
	 * absolute (or at least consistently-scaled) timestamp.
	 */
	uint64_t ts_be    = to_be64((uint64_t)ts32 * 100U + BTSNOOP_EPOCH_OFFSET);

	(void)fwrite(&orig_be,  sizeof(orig_be),  1U, monitor_file);
	(void)fwrite(&orig_be,  sizeof(orig_be),  1U, monitor_file); /* incl_len == orig_len */
	(void)fwrite(&flags_be, sizeof(flags_be), 1U, monitor_file);
	(void)fwrite(&zero32,   sizeof(zero32),   1U, monitor_file); /* drops */
	(void)fwrite(&ts_be,    sizeof(ts_be),    1U, monitor_file);
	if (len > 0U) {
		(void)fwrite(data, 1U, len, monitor_file);
	}
	(void)fflush(monitor_file);
}

/*
 * Inspect the frame buffer for a complete monitor wire-protocol frame.
 * If one is found, convert it to a btsnoop record and write it to the
 * output file, then remove the consumed bytes from the buffer.
 *
 * Frame layout (all multi-byte fields are little-endian):
 *   offset 0 : data_len  (uint16_t) = 4 + hdr_len + payload_len
 *   offset 2 : opcode    (uint16_t)
 *   offset 4 : flags     (uint8_t)
 *   offset 5 : hdr_len   (uint8_t)
 *   offset 6 : ext[hdr_len]            <- extended headers (incl. TS32)
 *   offset 6+hdr_len : payload[payload_len]
 *
 *   Total frame bytes = 2 + data_len = MONITOR_BASE_HDR_LEN + hdr_len + payload_len
 */
static void flush_frame(void)
{
	uint16_t data_len;
	uint16_t opcode;
	uint8_t  hdr_len;
	size_t   frame_size;
	uint32_t payload_len;
	uint32_t ts32 = 0U;
	const uint8_t *payload;

	if (frame_buf_len < MONITOR_BASE_HDR_LEN) {
		return;
	}

	data_len  = (uint16_t)frame_buf[0] | ((uint16_t)frame_buf[1] << 8);
	opcode    = (uint16_t)frame_buf[2] | ((uint16_t)frame_buf[3] << 8);
	hdr_len   = frame_buf[5];
	frame_size = 2U + (size_t)data_len;

	/* Sanity-check frame dimensions before touching the buffer */
	if (frame_size > FRAME_BUF_SIZE || data_len < 4U + (uint16_t)hdr_len) {
		nsi_print_warning("BT monitor: malformed frame (data_len=%u, "
				  "hdr_len=%u), discarding buffer\n",
				  (unsigned int)data_len, (unsigned int)hdr_len);
		frame_buf_len = 0U;
		return;
	}

	if (frame_buf_len < frame_size) {
		return; /* Frame not yet complete */
	}

	payload_len = data_len - 4U - hdr_len;
	payload     = frame_buf + MONITOR_BASE_HDR_LEN + hdr_len;

	/* TS32 is always the first extended-header entry when present
	 * (type = MONITOR_TS32, value = 4-byte LE uint32_t).
	 */
	if (hdr_len >= 5U && frame_buf[MONITOR_BASE_HDR_LEN] == MONITOR_TS32) {
		ts32 = (uint32_t)frame_buf[MONITOR_BASE_HDR_LEN + 1]
		     | ((uint32_t)frame_buf[MONITOR_BASE_HDR_LEN + 2] << 8)
		     | ((uint32_t)frame_buf[MONITOR_BASE_HDR_LEN + 3] << 16)
		     | ((uint32_t)frame_buf[MONITOR_BASE_HDR_LEN + 4] << 24);
	}

	write_btsnoop_record(opcode, ts32, payload, payload_len);

	/* Consume the processed frame from the buffer */
	frame_buf_len -= frame_size;
	if (frame_buf_len > 0U) {
		memmove(frame_buf, frame_buf + frame_size, frame_buf_len);
	}
}

void monitor_native_open(const char *path)
{
	monitor_file = fopen(path, "wb");
	if (monitor_file == NULL) {
		nsi_print_warning("BT monitor: failed to open '%s'\n", path);
		return;
	}
	frame_buf_len = 0U;
	write_btsnoop_file_header();
}

void monitor_native_write(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	size_t to_copy;

	if (monitor_file == NULL) {
		return;
	}

	while (len > 0U) {
		to_copy = FRAME_BUF_SIZE - frame_buf_len;
		if (to_copy == 0U) {
			nsi_print_warning("BT monitor: frame buffer overflow, "
					  "discarding buffered data\n");
			frame_buf_len = 0U;
			to_copy = FRAME_BUF_SIZE;
		}
		if (to_copy > len) {
			to_copy = len;
		}
		memcpy(frame_buf + frame_buf_len, bytes, to_copy);
		frame_buf_len += to_copy;
		bytes += to_copy;
		len   -= to_copy;
		flush_frame();
	}
}

void monitor_native_close(void)
{
	if (monitor_file != NULL) {
		(void)fclose(monitor_file);
		monitor_file  = NULL;
		frame_buf_len = 0U;
	}
}
