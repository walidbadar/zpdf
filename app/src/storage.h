/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Where documents come from. Two implementations: the mounted file system
 * (SD card) and host files on native_sim.
 */
#ifndef ZPDF_APP_STORAGE_H
#define ZPDF_APP_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*storage_list_cb)(const char *path, const char *name, void *user_data);

/* Mount or otherwise prepare the storage. Returns 0 or a negative errno. */
int storage_init(void);

/* Call cb for every PDF document found. Returns the count or a negative errno. */
int storage_list(storage_list_cb cb, void *user_data);

/*
 * Read a whole document into a buffer from the PDF allocator (PSRAM on the
 * ESP32-S3). Free it with storage_free(). Returns 0 or a negative errno.
 */
int storage_read(const char *path, uint8_t **buf, size_t *len);
void storage_free(uint8_t *buf);

/* True when the first document should be opened without user interaction. */
bool storage_auto_open(void);

/* One line describing the storage for the user interface. */
const char *storage_describe(void);

#endif
