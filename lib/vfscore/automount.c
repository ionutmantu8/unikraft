/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VFS Automatic Mounts
 *
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *          Robert Hrusecky <roberth@cs.utexas.edu>
 *          Omar Jamil <omarj2898@gmail.com>
 *          Sachin Beldona <sachinbeldona@utexas.edu>
 *          Sergiu Moga <sergiu@unikraft.io>
 *
 * Copyright (c) 2019, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2023, Unikraft GmbH. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mount.h>
#include <uk/assert.h>
#ifdef CONFIG_LIBUKCPIO
#include <uk/cpio.h>
#endif /* CONFIG_LIBUKCPIO */
#include <uk/init.h>
#include <uk/libparam.h>
#include <uk/plat/memory.h>

#ifdef CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS
#include <errno.h>
#include <uk/config.h>
#include <uk/arch/types.h>
#include <sys/stat.h>
#endif /* CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */

struct vfscore_volume {
	/* Volume source device */
	const char *sdev;
	/* Mount point absolute path */
	const char *path;
	/* Corresponding filesystem driver name */
	const char *drv;
	/* Mount flags */
	unsigned long flags;
	/* Mount options */
	const char *opts;
};

static inline int vfscore_mount_volume(struct vfscore_volume *vv)
{
	return mount(vv->sdev, vv->path, vv->drv, vv->flags, vv->opts);
}

#ifdef CONFIG_LIBVFSCORE_FSTAB

#define LIBVFSCORE_FSTAB_VOLUME_ARGS_SEP			':'

static char *vfscore_fstab[CONFIG_LIBVFSCORE_FSTAB_SIZE];

UK_LIBPARAM_PARAM_ARR_ALIAS(fstab, &vfscore_fstab, charp,
			    CONFIG_LIBVFSCORE_FSTAB_SIZE,
			    "VFSCore Filesystem Table");

/**
 * Expected command-line argument format:
 *	vfs.fstab=[
 *		"<src_dev>:<mntpoint>:<fsdriver>[:<flags>:<opts>]"
 *		"<src_dev>:<mntpoint>:<fsdriver>[:<flags>:<opts>]"
 *		...
 *	]
 * These list elements are expected to be separated by whitespaces.
 * Mount options and flags are optional.
 */
static char *vfscore_fstab_next_volume_arg(char **pos)
{
	char *ipos;

	UK_ASSERT(pos);

	if (!*pos)
		return NULL;

	ipos = *pos;
	*pos = strchr(ipos, LIBVFSCORE_FSTAB_VOLUME_ARGS_SEP);

	if (*pos && **pos != '\0') {
		**pos = '\0';
		(*pos)++;
	} else {
		*pos = NULL;
	}

	return ipos;
}

static void vfscore_fstab_fetch_volume_args(char *v, struct vfscore_volume *vv)
{
	const char *flags;
	char *pos;

	UK_ASSERT(v);
	UK_ASSERT(vv);

	pos = v;

	/* sdev, path and drv are mandatory */
	vv->sdev = vfscore_fstab_next_volume_arg(&pos);
	UK_ASSERT(vv->sdev);

	vv->path = vfscore_fstab_next_volume_arg(&pos);
	UK_ASSERT(vv->path);

	vv->drv = vfscore_fstab_next_volume_arg(&pos);
	UK_ASSERT(vv->drv);

	if (!pos)
		return;

	/* Allow empty flags: "<src_dev>:<mntpoint>:<fsdriver>::<opts>" */
	flags = vfscore_fstab_next_volume_arg(&pos);
	if (flags && flags[0] != '\0')
		vv->flags = strtol(flags, NULL, 0);
	else
		vv->flags = 0;

	vv->opts = vfscore_fstab_next_volume_arg(&pos);
	if (vv->opts && vv->opts[0] == '\0')
		vv->opts = NULL;
}
#endif /* CONFIG_LIBVFSCORE_FSTAB */

#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
static int do_mount_initrd(const void *initrd, size_t len, const char *path)
{
	int rc;

	UK_ASSERT(path);

	rc = mount("", path, "ramfs", 0x0, NULL);
	if (unlikely(rc)) {
		uk_pr_crit("Failed to mount ramfs to \"%s\": %d\n",
			   path, errno);
		return -1;
	}

	uk_pr_info("Extracting initrd @ %p (%"__PRIsz" bytes) to %s...\n",
		   (void *)initrd, len, path);
	rc = ukcpio_extract(path, (void *)initrd, len);
	if (unlikely(rc)) {
		uk_pr_crit("Failed to extract cpio archive to %s: %d\n",
			   path, rc);
		return -1;
	}

	return 0;
}
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */

#if CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS
#if CONFIG_LIBVFSCORE_ROOTFS_EINITRD
extern const char vfscore_einitrd_start[];
extern const char vfscore_einitrd_end;

static int vfscore_automount_rootfs(void)
{
	const void *initrd;
	size_t len;

	initrd = (const void *)vfscore_einitrd_start;
	len    = (size_t)((uintptr_t)&vfscore_einitrd_end
			  - (uintptr_t)vfscore_einitrd_start);

	return do_mount_initrd(initrd, len, "/");
}

#else /* !CONFIG_LIBVFSCORE_ROOTFS_EINITRD */
#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
static int vfscore_mount_initrd_volume(struct vfscore_volume *vv)
{
	struct ukplat_memregion_desc *initrd;
	int rc;

	UK_ASSERT(vv);

	/* TODO: Support multiple Initial RAM Disks */
	rc = ukplat_memregion_find_initrd0(&initrd);
	if (unlikely(rc < 0)) {
		uk_pr_crit("Could not find an initrd!\n");

		return -1;
	}

	return do_mount_initrd((void *)initrd->vbase, initrd->len,
			       vv->path);
}
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */

static int vfscore_automount_rootfs(void)
{
	/* Convert to `struct vfscore_volume` */
	struct vfscore_volume vv = {
#ifdef CONFIG_LIBVFSCORE_ROOTDEV
		.sdev = CONFIG_LIBVFSCORE_ROOTDEV,
#else
		.sdev = "",
#endif /* CONFIG_LIBVFSCORE_ROOTDEV */
		.path = "/",
		.drv = CONFIG_LIBVFSCORE_ROOTFS,
#ifdef CONFIG_LIBVFSCORE_ROOTFLAGS
		.flags = CONFIG_LIBVFSCORE_ROOTFLAGS,
#else
		.flags = 0,
#endif /* CONFIG_LIBVFSCORE_ROOTFLAGS */
#ifdef CONFIG_LIBVFSCORE_ROOTOPTS
		.opts = CONFIG_LIBVFSCORE_ROOTOPTS,
#else
		.opts = "",
#endif /* CONFIG_LIBVFSCORE_ROOTOPTS */
	};
	int rc;

	/*
	 * Initialization of the root filesystem '/'
	 * NOTE: Any additional sub mount points (like '/dev' with devfs)
	 * have to be mounted later.
	 *
	 * Silently return 0, as user might not have configured implicit rootfs.
	 */
	if (!vv.drv || vv.drv[0] == '\0')
		return 0;

#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
	if (!strncmp(vv.drv, "initrd", sizeof("initrd") - 1))
		return vfscore_mount_initrd_volume(&vv);
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */

	rc = vfscore_mount_volume(&vv);
	if (unlikely(rc))
		uk_pr_crit("Failed to mount %s (%s) at /: %d\n", vv.sdev,
			   vv.drv, rc);

	return rc;
}
#endif /* !CONFIG_LIBVFSCORE_ROOTFS_EINITRD */
#else /* CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */
static int vfscore_automount_rootfs(void)
{
	return 0;
}
#endif /* !CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */

#ifdef CONFIG_LIBVFSCORE_FSTAB
static int vfscore_automount_fstab_volumes(void)
{
	struct vfscore_volume vv;
	int rc, i;

	for (i = 0; i < CONFIG_LIBVFSCORE_FSTAB_SIZE && vfscore_fstab[i]; i++) {
		vfscore_fstab_fetch_volume_args(vfscore_fstab[i], &vv);

#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
		if (!strncmp(vv.drv, "initrd", sizeof("initrd") - 1)) {
			rc = vfscore_mount_initrd_volume(&vv);
		} else
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */
		{
			rc = vfscore_mount_volume(&vv);
		}
		if (unlikely(rc)) {
			uk_pr_err("Failed to mount %s: error %d\n", vv.sdev,
				  rc);

			return rc;
		}
	}

	return 0;
}
#else /* CONFIG_LIBVFSCORE_FSTAB */
static int vfscore_automount_fstab_volumes(void)
{
	return 0;
}
#endif /* !CONFIG_LIBVFSCORE_FSTAB */

static int vfscore_automount(void)
{
	int rc;

	rc = vfscore_automount_rootfs();
	if (unlikely(rc < 0))
		return rc;

	return vfscore_automount_fstab_volumes();
}

uk_rootfs_initcall_prio(vfscore_automount, 4);
