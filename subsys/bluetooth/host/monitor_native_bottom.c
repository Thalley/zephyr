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
 */

/* Note: This is used only for interaction with the host C library, and is
 * therefore exempt of coding guidelines rule A.4&5 which applies to the
 * embedded code using embedded libraries.
 */

#include <stddef.h>
#include <stdio.h>
#include <nsi_tracing.h>

static FILE *monitor_file;

void monitor_native_open(const char *path)
{
	monitor_file = fopen(path, "wb");
	if (monitor_file == NULL) {
		nsi_print_warning("BT monitor: failed to open '%s'\n", path);
	}
}

void monitor_native_write(const void *data, size_t len)
{
	if (monitor_file == NULL) {
		return;
	}

	(void)fwrite(data, 1, len, monitor_file);
	(void)fflush(monitor_file);
}

void monitor_native_close(void)
{
	if (monitor_file != NULL) {
		(void)fclose(monitor_file);
		monitor_file = NULL;
	}
}
