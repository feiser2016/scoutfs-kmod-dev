/*
 * Copyright (C) 2015 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/magic.h>
#include <linux/buffer_head.h>
#include <linux/crc32c.h>
#include <linux/random.h>

#include "super.h"
#include "format.h"
#include "inode.h"
#include "dir.h"
#include "msg.h"

static const struct super_operations scoutfs_super_ops = {
	.alloc_inode = scoutfs_alloc_inode,
	.destroy_inode = scoutfs_destroy_inode,
};

static int read_supers(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct scoutfs_super *super;
	int found = -1;
	u32 crc;
	int i;

	for (i = 0; i < 2; i++) {
		if (bh)
			brelse(bh);
		bh = sb_bread(sb, SCOUTFS_SUPER_BRICK + i);
		if (!bh) {
			scoutfs_warn(sb, "couldn't read super brick %u", i);
			continue;
		}

		super = (void *)bh->b_data;

		if (super->id != cpu_to_le64(SCOUTFS_SUPER_ID)) {
			scoutfs_warn(sb, "super brick %u has invalid id %llx",
				     i, le64_to_cpu(super->id));
			continue;
		}

		crc = crc32c(~0, (char *)&super->hdr.crc + sizeof(crc),
			     SCOUTFS_BRICK_SIZE - sizeof(crc));
		if (crc != le32_to_cpu(super->hdr.crc)) {
			scoutfs_warn(sb, "super brick %u has bad crc %x (expected %x)",
				     i, crc, le32_to_cpu(super->hdr.crc));
			continue;
		}

		if (found < 0 || (le64_to_cpu(super->hdr.seq) >
				le64_to_cpu(sbi->super.hdr.seq))) {
			memcpy(&sbi->super, super,
			       sizeof(struct scoutfs_super));
			found = i;
		}
	}

	if (bh)
		brelse(bh);

	if (found < 0) {
		scoutfs_err(sb, "unable to read valid super brick");
		return -EINVAL;
	}

	scoutfs_info(sb, "using super %u with seq %llu",
		     found, le64_to_cpu(sbi->super.hdr.seq));

	/*
	 * XXX These don't exist in the super yet.  They should soon.
	 */
	atomic64_set(&sbi->next_ino, SCOUTFS_ROOT_INO + 1);
	atomic64_set(&sbi->next_blkno, 2);

	for (i = 0; i < ARRAY_SIZE(sbi->bloom_hash_keys); i++) {
		get_random_bytes(&sbi->bloom_hash_keys[i],
				 sizeof(sbi->bloom_hash_keys[i]));
	}

	return 0;
}

static int scoutfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct scoutfs_sb_info *sbi;
	struct inode *inode;
	int ret;

	sb->s_magic = SCOUTFS_SUPER_MAGIC;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &scoutfs_super_ops;

	sbi = kzalloc(sizeof(struct scoutfs_sb_info), GFP_KERNEL);
	sb->s_fs_info = sbi;
	if (!sbi)
		return -ENOMEM;

	spin_lock_init(&sbi->item_lock);
	sbi->item_root = RB_ROOT;
	sbi->dirty_item_root = RB_ROOT;

	if (!sb_set_blocksize(sb, SCOUTFS_BRICK_SIZE)) {
		printk(KERN_ERR "couldn't set blocksize\n");
		return -EINVAL;
	}

	ret = read_supers(sb);
	if (ret)
		return ret;

	inode = scoutfs_iget(sb, SCOUTFS_ROOT_INO);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static struct dentry *scoutfs_mount(struct file_system_type *fs_type, int flags,
				    const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, scoutfs_fill_super);
}

static void scoutfs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
	kfree(sb->s_fs_info);
}

static struct file_system_type scoutfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "scoutfs",
	.mount		= scoutfs_mount,
	.kill_sb	= scoutfs_kill_sb,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init scoutfs_module_init(void)
{
	return scoutfs_inode_init() ?:
	       scoutfs_dir_init() ?:
	       register_filesystem(&scoutfs_fs_type);
}
module_init(scoutfs_module_init)

static void __exit scoutfs_module_exit(void)
{
	unregister_filesystem(&scoutfs_fs_type);
	scoutfs_dir_exit();
	scoutfs_inode_exit();
}
module_exit(scoutfs_module_exit)

MODULE_AUTHOR("Zach Brown <zab@versity.com>");
MODULE_LICENSE("GPL");
