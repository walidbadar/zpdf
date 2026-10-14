/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "storage.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <string.h>
#include <errno.h>

/* The library allocator puts large buffers in external RAM. */
void *pdf_malloc(size_t n);
void pdf_free(void *p);

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = CONFIG_ZPDF_APP_MOUNT_POINT,
	/* never format a card that cannot be read; the user's data is on it */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static bool mounted;
static char describe_buf[48];

int storage_init(void)
{
	uint32_t sectors = 0, sector_size = 0;

	if (mounted) {
		return 0;
	}
	int rc = disk_access_ioctl(CONFIG_ZPDF_APP_DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);

	if (rc != 0) {
		LOG_ERR("no SD card (%d)", rc);
		snprintf(describe_buf, sizeof(describe_buf), "No SD card found");
		return rc;
	}
	disk_access_ioctl(CONFIG_ZPDF_APP_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
	disk_access_ioctl(CONFIG_ZPDF_APP_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);

	rc = fs_mount(&mp);
	if (rc != 0) {
		LOG_ERR("mount %s failed (%d)", mp.mnt_point, rc);
		snprintf(describe_buf, sizeof(describe_buf), "SD card: cannot mount (%d)", rc);
		return rc;
	}
	mounted = true;
	uint32_t mb = (uint32_t)(((uint64_t)sectors * sector_size) >> 20);

	snprintf(describe_buf, sizeof(describe_buf), "SD card, %u MB", mb);
	LOG_INF("mounted %s (%u MB)", mp.mnt_point, mb);
	return 0;
}

static bool is_pdf(const char *name)
{
	size_t n = strlen(name);

	if (n < 4) {
		return false;
	}
	const char *ext = name + n - 4;

	return (ext[0] == '.') && (ext[1] == 'p' || ext[1] == 'P') && (ext[2] == 'd' || ext[2] == 'D') &&
	       (ext[3] == 'f' || ext[3] == 'F');
}

int storage_list(storage_list_cb cb, void *user_data)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	char path[CONFIG_FS_FATFS_MAX_LFN + 16];
	int count = 0;

	if (!mounted) {
		return -ENODEV;
	}
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, CONFIG_ZPDF_APP_MOUNT_POINT);

	if (rc != 0) {
		return rc;
	}
	while (count < CONFIG_ZPDF_APP_MAX_FILES) {
		rc = fs_readdir(&dir, &ent);
		if (rc != 0 || ent.name[0] == 0) {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_FILE || !is_pdf(ent.name)) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", CONFIG_ZPDF_APP_MOUNT_POINT, ent.name);
		cb(path, ent.name, user_data);
		count++;
	}
	fs_closedir(&dir);
	return count;
}

int storage_read(const char *path, uint8_t **buf, size_t *len)
{
	struct fs_file_t file;
	struct fs_dirent ent;

	*buf = NULL;
	*len = 0;
	int rc = fs_stat(path, &ent);

	if (rc != 0) {
		return rc;
	}
	if (ent.size == 0) {
		return -EINVAL;
	}
	if (ent.size > CONFIG_ZPDF_APP_MAX_FILE_SIZE) {
		return -EFBIG;
	}
	uint8_t *data = pdf_malloc(ent.size + 1);

	if (data == NULL) {
		return -ENOMEM;
	}
	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		pdf_free(data);
		return rc;
	}
	size_t got = 0;

	while (got < ent.size) {
		ssize_t n = fs_read(&file, data + got, ent.size - got);

		if (n < 0) {
			rc = (int)n;
			break;
		}
		if (n == 0) {
			break;
		}
		got += (size_t)n;
	}
	fs_close(&file);
	if (rc != 0 || got == 0) {
		pdf_free(data);
		return rc != 0 ? rc : -EIO;
	}
	data[got] = 0;
	*buf = data;
	*len = got;
	return 0;
}

void storage_free(uint8_t *buf)
{
	pdf_free(buf);
}

bool storage_auto_open(void)
{
	return IS_ENABLED(CONFIG_ZPDF_APP_AUTO_OPEN) && mounted;
}

const char *storage_describe(void)
{
	return describe_buf;
}
