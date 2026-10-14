/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * native_sim: documents are files of the host, named on the command line:
 *   zephyr.exe --pdf=/path/one.pdf --pdf=/path/two.pdf
 */
#include "storage.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <posix_native_task.h>
#include <cmdline.h>
#include <nsi_host_trampolines.h>
#include <string.h>
#include <errno.h>

void *pdf_malloc(size_t n);
void *pdf_realloc(void *p, size_t n);
void pdf_free(void *p);

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

static char *pdf_arg;
static bool auto_open;
static const char *files[CONFIG_ZPDF_APP_MAX_FILES];
static int nfiles;

static void pdf_arg_found(char *argv, int offset)
{
	ARG_UNUSED(argv);
	ARG_UNUSED(offset);
	if (nfiles < CONFIG_ZPDF_APP_MAX_FILES && pdf_arg != NULL) {
		files[nfiles++] = nsi_host_strdup(pdf_arg);
	}
}

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{
			.option = "pdf",
			.name = "path",
			.type = 's',
			.dest = (void *)&pdf_arg,
			.call_when_found = pdf_arg_found,
			.descript = "PDF document to list in the reader (may be repeated)",
		},
		{
			.is_switch = true,
			.option = "open",
			.type = 'b',
			.dest = (void *)&auto_open,
			.descript = "Open the first document right away (for testing)",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 10);

int storage_init(void)
{
	return nfiles > 0 ? 0 : -ENOENT;
}

int storage_list(storage_list_cb cb, void *user_data)
{
	for (int i = 0; i < nfiles; i++) {
		const char *slash = strrchr(files[i], '/');

		cb(files[i], slash ? slash + 1 : files[i], user_data);
	}
	return nfiles;
}

int storage_read(const char *path, uint8_t **buf, size_t *len)
{
	*buf = NULL;
	*len = 0;
	int fd = nsi_host_open(path, 0 /* O_RDONLY */);

	if (fd < 0) {
		return -ENOENT;
	}
	size_t cap = 256 * 1024, n = 0;
	uint8_t *data = pdf_malloc(cap + 1);

	if (data == NULL) {
		nsi_host_close(fd);
		return -ENOMEM;
	}
	for (;;) {
		if (n == cap) {
			if (cap >= CONFIG_ZPDF_APP_MAX_FILE_SIZE) {
				pdf_free(data);
				nsi_host_close(fd);
				return -EFBIG;
			}
			cap *= 2;
			uint8_t *d = pdf_realloc(data, cap + 1);

			if (d == NULL) {
				pdf_free(data);
				nsi_host_close(fd);
				return -ENOMEM;
			}
			data = d;
		}
		long r = nsi_host_read(fd, data + n, cap - n);

		if (r < 0) {
			pdf_free(data);
			nsi_host_close(fd);
			return -EIO;
		}
		if (r == 0) {
			break;
		}
		n += (size_t)r;
	}
	nsi_host_close(fd);
	if (n == 0) {
		pdf_free(data);
		return -EINVAL;
	}
	data[n] = 0;
	*buf = data;
	*len = n;
	return 0;
}

void storage_free(uint8_t *buf)
{
	pdf_free(buf);
}

bool storage_auto_open(void)
{
	return auto_open && nfiles > 0;
}

const char *storage_describe(void)
{
	return nfiles > 0 ? "Host files (--pdf=...)" : "No documents: run with --pdf=<file>";
}
