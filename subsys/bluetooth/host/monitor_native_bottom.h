/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bottom-layer interface for the native Bluetooth monitor file backend.
 *
 * Functions declared here are implemented in monitor_native_bottom.c,
 * which is compiled in the native_simulator runner context against the host
 * C library.  They may be called freely from the Zephyr-side monitor.c.
 */

#ifndef SUBSYS_BLUETOOTH_HOST_MONITOR_NATIVE_BOTTOM_H
#define SUBSYS_BLUETOOTH_HOST_MONITOR_NATIVE_BOTTOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the Bluetooth monitor output file.
 *
 * @param path  File path to open for writing.  Must not be NULL.
 */
void monitor_native_open(const char *path);

/**
 * @brief Write bytes to the Bluetooth monitor output file.
 *
 * Has no effect if the file has not been opened successfully.
 *
 * @param data  Pointer to the data to write.
 * @param len   Number of bytes to write.
 */
void monitor_native_write(const void *data, size_t len);

/**
 * @brief Flush and close the Bluetooth monitor output file.
 */
void monitor_native_close(void);

#ifdef __cplusplus
}
#endif

#endif /* SUBSYS_BLUETOOTH_HOST_MONITOR_NATIVE_BOTTOM_H */
