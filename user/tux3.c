/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "inode.c"
#include <getopt.h>

void change_begin(struct sb *sb) { }
void change_end(struct sb *sb) { }

int fls(uint32_t v)
{
	uint32_t mask;
	int bit = 0;
	for (bit = 32, mask = 1 << 31; bit; mask >>= 1, bit--)
		if ((v & mask))
			break;
	return bit;
}

static void usage(void)
{
	printf("tux3 [-s|--seek=<offset>] [-b|--blocksize=<size>] [-h|--help]\n"
	       "     <command> <volume> [<file>]\n");
	exit(1);
}

static int mkfs(int fd, const char *volname, unsigned blocksize)
{
	u64 volsize = 0;
	if (fdsize64(fd, &volsize))
		error("fdsize64 failed for '%s' (%s)", volname, strerror(errno));
	int blockbits = 12;
	if (blocksize) {
		blockbits = fls(blocksize) - 1;
		if (1 << blockbits != blocksize)
			error("blocksize must be a power of two");
	}
	struct dev *dev = &(struct dev){ .fd = fd, .bits = blockbits };
	init_buffers(dev, 1 << 20, 1);

	struct sb *sb = rapid_sb(dev,
		.max_inodes_per_block = 64,
		.entries_per_node = 20,
		.volblocks = volsize >> dev->bits,
		.freeblocks = volsize >> dev->bits);
	sb->super = (struct disksuper){ .magic = SB_MAGIC, .volblocks = to_be_u64(sb->blockbits) };

	sb->volmap = tux_new_volmap(sb);
	if (!sb->volmap)
		return -ENOMEM;

	printf("make tux3 filesystem on %s (0x%Lx bytes)\n", volname, (L)volsize);
	int err = make_tux3(sb);
	if (!err)
		show_tree_range(itable_btree(sb), 0, -1);
	return err;
}

int main(int argc, char *argv[])
{
	char *seekarg = NULL;
	unsigned blocksize = 0;
	static struct option long_options[] = {
		{ "seek", required_argument, 0, 's' },
		{ "blocksize", required_argument, 0, 'b' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	while (1) {
		int c, optindex = 0;
		c = getopt_long(argc, argv, "s:b:h", long_options, &optindex);
		if (c == -1)
			break;
		switch (c) {
		case 's':
			seekarg = optarg;
			break;
		case 'b':
			blocksize = strtoul(optarg, NULL, 0);
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (argc - optind < 2)
		goto usage;

	/* open volume, create superblock */
	const char *command = argv[optind++];
	const char *volname = argv[optind++];
	int fd = open(volname, O_RDWR, S_IRWXU);
	if (fd < 0)
		goto eek;

	if (!strcmp(command, "mkfs") || !strcmp(command, "make")) {
		if (optind != argc)
			goto usage;
		if ((errno = -mkfs(fd, volname, blocksize)))
			goto eek;
		return 0;
	}

	/* dev->bits is still unknown. Note, some structure can't use yet. */
	struct dev *dev = &(struct dev){ .fd = fd };
	struct sb *sb = rapid_sb(dev);
	if ((errno = -load_sb(sb)))
		goto eek;
	dev->bits = sb->blockbits;
	init_buffers(dev, 1 << 20, 1);

	sb->volmap = tux_new_volmap(sb);
	if (!sb->volmap)
		goto eek;
	if ((errno = -load_itable(sb)))
		goto eek;
	if (!(sb->bitmap = iget(sb, TUX_BITMAP_INO)))
		goto eek;
	if (!(sb->rootdir = iget(sb, TUX_ROOTDIR_INO)))
		goto eek;
	if (!(sb->atable = iget(sb, TUX_ATABLE_INO)))
		goto eek;
	if ((errno = -open_inode(sb->bitmap)))
		goto eek;
	if ((errno = -open_inode(sb->rootdir)))
		goto eek;
	if ((errno = -open_inode(sb->atable)))
		goto eek;
	show_tree_range(&sb->rootdir->btree, 0, -1);
	show_tree_range(&sb->bitmap->btree, 0, -1);

	if (argc - optind < 1)
		goto usage;
	char *filename = argv[optind++];

	if (!strcmp(command, "write")) {
		printf("---- open file ----\n");
		struct inode *inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (!inode) {
			printf("---- create file ----\n");
			inode = tuxcreate(sb->rootdir, filename, strlen(filename),
					  &(struct tux_iattr){ .mode = S_IFREG | S_IRWXU });
			if (!inode) {
				errno = EEXIST;
				goto eek;
			}
		}
		tux_dump_entries(blockget(sb->rootdir->map, 0));
		printf("---- write file ----\n");
		struct file *file = &(struct file){ .f_inode = inode };

		struct stat stat;
		if ((fstat(0, &stat)) == -1)
			goto eek;
		if (seekarg) {
			loff_t seek = strtoull(seekarg, NULL, 0);
			printf("seek to %Li\n", (L)seek);
			tuxseek(file, seek);
		}
		char text[1 << 16];
		while (1) {
			int len = read(0, text, sizeof(text));
			if (len < 0)
				goto eek;
			if (!len)
				break;
			if ((errno = -tuxwrite(file, text, len)) > 0)
				goto eek;
		}
		if ((errno = -tuxsync(inode)))
			goto eek;
		if ((errno = -sync_super(sb)))
			goto eek;
		//bitmap_dump(sb->bitmap, 0, sb->volblocks);
		tux_dump_entries(blockget(sb->rootdir->map, 0));
		//show_tree_range(&sb->itable, 0, -1);
	}

	if (!strcmp(command, "read")) {
		printf("---- read file ----\n");
		//show_tree_range(&sb->itable, 0, -1);
		//tux_dump_entries(blockread(sb->rootdir->map, 0));
		struct inode *inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (!inode) {
			errno = ENOENT;
			goto eek;
		}
		struct file *file = &(struct file){ .f_inode = inode };
		char buf[100] = { };
		if (seekarg) {
			loff_t seek = strtoull(seekarg, NULL, 0);
			printf("seek to %Li\n", (L)seek);
			tuxseek(file, seek);
		}
		memset(buf, 0, sizeof(buf));
		int got = tuxread(file, buf, sizeof(buf));
		//printf("got %x bytes\n", got);
		if (got < 0)
			return 1;
		hexdump(buf, got);
	}

	if (!strcmp(command, "get") || !strcmp(command, "set")) {
		printf("---- read attribute ----\n");
		struct inode *inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (!inode) {
			errno = ENOENT;
			goto eek;
		}
		if (argc - optind < 1)
			goto usage;
		char *name = argv[optind++];
		if (!strcmp(command, "get")) {
			printf("read xattr %.*s\n", (int)strlen(name), name);
			int size = get_xattr(inode, name, strlen(name), NULL, 0);
			if (size < 0) {
				errno = -size;
				goto eek;
			}
			void *data = malloc(size);
			if (!data) {
				errno = ENOMEM;
				goto eek;
			}
			size = get_xattr(inode, name, strlen(name), data, size);
			if (size < 0) {
				free(data);
				errno = -size;
				goto eek;
			}
			hexdump(data, size);
			free(data);
		}
		if (!strcmp(command, "set")) {
			char text[2 << 16];
			unsigned len;
			len = read(0, text, sizeof(text));
			printf("got %i bytes\n", len);
			if ((errno = -set_xattr(inode, "foo", 3, "foobar", 6, 0)))
				goto eek;
			tuxsync(inode);
			if ((errno = -sync_super(sb)))
				goto eek;
		}
	}

	if (!strcmp(command, "stat")) {
		printf("---- stat file ----\n");
		struct inode *inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (!inode) {
			errno = ENOENT;
			goto eek;
		}
		dump_attrs(inode);
		free_inode(inode);
	}

	if (!strcmp(command, "delete")) {
		printf("---- delete file ----\n");
		if ((errno = -tuxunlink(sb->rootdir, filename, strlen(filename))))
			goto eek;
		tux_dump_entries(blockread(sb->rootdir->map, 0));
		if ((errno = -sync_super(sb)))
			goto eek;
	}

	if (!strcmp(command, "truncate")) {
		printf("---- truncate file ----\n");
		struct inode *inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (!inode) {
			errno = ENOENT;
			goto eek;
		}
		loff_t seek = 0;
		if (seekarg)
			seek = strtoull(seekarg, NULL, 0);
		printf("---- new size %Lu ----\n", (L)seek);
		if ((errno = -tuxtruncate(inode, seek)))
			goto eek;
		tuxsync(inode);
		if ((errno = -sync_super(sb)))
			goto eek;
	}

	//printf("---- show state ----\n");
	//show_buffers(sb->rootdir->map);
	//show_buffers(sb->volmap->map);
	exit(0);
	return 0;
eek:
	fprintf(stderr, "%s!\n", strerror(errno));
	exit(1);
usage:
	usage();
	exit(1);
}
