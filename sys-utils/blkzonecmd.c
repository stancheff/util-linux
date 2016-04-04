/*
 * blkreport.c -- request a zone report on part (or all) of the block device.
 *
 * Copyright (C) 2015 Seagate Technology PLC
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

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fs.h>

#ifdef HAVE_BLKZONED_API_H
#include <linux/blkzoned_api.h>
#endif

#include "nls.h"
#include "strutils.h"
#include "c.h"
#include "closestream.h"
#include "monotonic.h"

#ifndef BLKDISCARD
# define BLKDISCARD	_IO(0x12,119)
#endif

#ifndef BLKOPENZONE
# define BLKOPENZONE	_IO(0x12, 131)
#endif

#ifndef BLKCLOSEZONE
# define BLKCLOSEZONE	_IO(0x12, 132)
#endif

#ifndef BLKRESETZONE
# define BLKRESETZONE	_IO(0x12, 133)
#endif

enum {
	ACT_OPEN_ZONE = 0,
	ACT_CLOSE_ZONE,
	ACT_RESET_ZONE,
};

static void print_stats(int act, char *path, uint64_t lba)
{
	switch (act) {
	case ACT_RESET_ZONE:
		printf(_("%s: Reset Zone %" PRIu64 "\n"), path, lba);
		break;
	case ACT_OPEN_ZONE:
		printf(_("%s: Open Zone %" PRIu64 "\n"), path, lba);
		break;
	case ACT_CLOSE_ZONE:
		printf(_("%s: Close Zone %" PRIu64 "\n"), path, lba);
		break;
	default:
		printf(_("%s: Unknown Action on %" PRIu64 "\n"), path, lba);
		break;
	}
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Discard the content of sectors on a device.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -z, --zone <num>    lba of start of zone to act upon\n"
		" -o, --open          open zone\n"
		" -c, --close         close zone\n"
		" -r, --reset         reset zone\n"
		" -a, --ata           use ata passthrough\n"
		" -v, --verbose       print aligned length and offset\n"),
		out);
	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("blkzonecmd(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}


int main(int argc, char **argv)
{
	char *path;
	int c, fd, verbose = 0, secsize;
	uint64_t blksize;
	struct stat sb;
	uint64_t zone_lba = 0ul;
	int ata = 0;
	int act = ACT_OPEN_ZONE;

	static const struct option longopts[] = {
	    { "help",      0, 0, 'h' },
	    { "version",   0, 0, 'V' },
	    { "zone",      1, 0, 'z' },
	    { "open",      0, 0, 'o' },
	    { "close",     0, 0, 'c' },
	    { "reset",     0, 0, 'r' },
	    { "ata",       0, 0, 'a' },
	    { "verbose",   0, 0, 'v' },
	    { NULL,        0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	zone_lba = 0;

	while ((c = getopt_long(argc, argv, "hVaocrz:", longopts, NULL)) != -1) {
		switch(c) {
		case 'h':
			usage(stdout);
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'z':
			zone_lba = strtou64_base_or_err(0, optarg,
					_("failed to parse length"));
			break;
		case 'a':
			ata = 1;
			break;
		case 'o':
			act = ACT_OPEN_ZONE;
			break;
		case 'c':
			act = ACT_CLOSE_ZONE;
			break;
		case 'r':
			act = ACT_RESET_ZONE;
			break;
		case 'v':
			verbose = 1;
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

	fd = open(path, O_WRONLY);
	if (fd < 0)
		err(EXIT_FAILURE, _("cannot open %s"), path);

	if (fstat(fd, &sb) == -1)
		err(EXIT_FAILURE, _("stat of %s failed"), path);
	if (!S_ISBLK(sb.st_mode))
		errx(EXIT_FAILURE, _("%s: not a block device"), path);

	if (ioctl(fd, BLKGETSIZE64, &blksize))
		err(EXIT_FAILURE, _("%s: BLKGETSIZE64 ioctl failed"), path);
	if (ioctl(fd, BLKSSZGET, &secsize))
		err(EXIT_FAILURE, _("%s: BLKSSZGET ioctl failed"), path);

	if (zone_lba != ~0ul) {
		/* check offset alignment to the sector size */
		if (zone_lba % secsize)
			errx(EXIT_FAILURE, _("%s: offset %" PRIu64 " is not aligned "
				 "to sector size %i"), path, zone_lba, secsize);

		/* is the range end behind the end of the device ?*/
		if (zone_lba > blksize)
			errx(EXIT_FAILURE, _("%s: offset is greater than device size"), path);
	}

	if (ata) {
		zone_lba |= 1ul; /* ensure low bit is set */
	} else {
		zone_lba &= ~1ul; /* ensure low bit is clear */
	}

	switch (act) {
	case ACT_OPEN_ZONE:
		if (ioctl(fd, BLKOPENZONE, zone_lba))
			 err(EXIT_FAILURE, _("%s: BLKOPENZONE ioctl failed"), path);
		break;
	case ACT_CLOSE_ZONE:
		if (ioctl(fd, BLKCLOSEZONE, zone_lba))
			err(EXIT_FAILURE, _("%s: BLKCLOSEZONE ioctl failed"), path);
		break;

	case ACT_RESET_ZONE:
		if (ioctl(fd, BLKRESETZONE, zone_lba))
			err(EXIT_FAILURE, _("%s: BLKRESETZONE ioctl failed"), path);
		break;
	}

	if (verbose && zone_lba)
		print_stats(act, path, zone_lba);

	close(fd);
	return EXIT_SUCCESS;
}
