/*
 * blkreset.c -- Reset the WP on a range of zones.
 *
 * Copyright (C) 2015,2016 Seagate Technology PLC
 * Written by Shaun Tancheff <shaun.tancheff@seagate.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This program uses BLKREPORT ioctl to query zone information about part of
 * or a whole block device, if the device supports it.
 * You can specify range (start and length) to be queried.
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <limits.h>
#include <getopt.h>
#include <time.h>
#include <ctype.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/blkzoned.h>

#include "sysfs.h"
#include "nls.h"
#include "strutils.h"
#include "c.h"
#include "closestream.h"
#include "monotonic.h"

static unsigned long get_zone_size(const char *dname)
{
	struct sysfs_cxt cxt = UL_SYSFSCXT_EMPTY;
	dev_t devno = sysfs_devname_to_devno(dname, NULL);
	int major_no = major(devno);
	int block_no = minor(devno) & ~0x0f;
	uint64_t sz;

	/*
	 * Mapping /dev/sdXn -> /sys/block/sdX to read the chunk_size entry.
	 * This method masks off the partition specified by the minor device
	 * component.
	 */
	devno = makedev(major_no, block_no);
	if (sysfs_init(&cxt, devno, NULL))
		return 0;

	if (sysfs_read_u64(&cxt, "queue/chunk_sectors", &sz) != 0)
		warnx(_("%s: failed to read chunk size"), dname);

	sysfs_deinit(&cxt);
	return sz;
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Discard the content of sectors on a device.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -z, --zone <num>  lba of start of zone to act upon\n"
		" -c, --count       number of zones to reset (default = 1)"),
		out);
	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("blkreset(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}


int main(int argc, char **argv)
{
	char *path;
	int c, fd;
	uint64_t blksize;
	uint64_t blksectors;
	struct stat sb;
	struct blk_zone_range za;
	uint64_t zsector = 0ul;
	uint64_t zlen = 0;
	uint64_t zcount = 1;
	unsigned long zsize;
	int rc = 0;

	static const struct option longopts[] = {
	    { "help",      0, 0, 'h' },
	    { "version",   0, 0, 'V' },
	    { "zone",      1, 0, 'z' },
	    { "count",     1, 0, 'c' },
	    { NULL,        0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "hVz:c:", longopts, NULL)) != -1) {
		switch(c) {
		case 'h':
			usage(stdout);
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'z':
			zsector = strtou64_base_or_err(0, optarg,
					_("failed to parse zone"));
			break;
		case 'c':
			zcount = strtou64_base_or_err(0, optarg,
					_("failed to parse number of zones"));
			break;
		default:
			usage(stderr);
			break;
		}
	}

	if (optind == argc)
		errx(EXIT_FAILURE, _("no device specified"));

	path = argv[optind++];

	if (optind != argc) {
		warnx(_("unexpected number of arguments"));
		usage(stderr);
	}

	zsize = get_zone_size(path);
	if (zsize == 0)
		err(EXIT_FAILURE, _("%s: Unable to determine zone size"), path);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		err(EXIT_FAILURE, _("cannot open %s"), path);

	if (fstat(fd, &sb) == -1)
		err(EXIT_FAILURE, _("stat of %s failed"), path);
	if (!S_ISBLK(sb.st_mode))
		errx(EXIT_FAILURE, _("%s: not a block device"), path);

	if (ioctl(fd, BLKGETSIZE64, &blksize))
		err(EXIT_FAILURE, _("%s: BLKGETSIZE64 ioctl failed"), path);

	blksectors = blksize << 9;

	/* check offset alignment to the chunk size */
	if (zsector & (zsize - 1))
		errx(EXIT_FAILURE, _("%s: zone %" PRIu64 " is not aligned "
			 "to zone size %" PRIu64), path, zsector, zsize);
	if (zsector > blksectors)
		errx(EXIT_FAILURE, _("%s: zone %" PRIu64 " is too large "
			 "for device %" PRIu64), path, zsector, blksectors);

	zlen = zcount * zsize;
	if (zsector + zlen > blksectors)
		zlen = blksectors - zsector;

	za.sector = zsector;
	za.nr_sectors = zlen;
	rc = ioctl(fd, BLKRESETZONE, &za);
	if (rc == -1)
		err(EXIT_FAILURE, _("%s: BLKRESETZONE ioctl failed"), path);

	close(fd);
	return EXIT_SUCCESS;
}
