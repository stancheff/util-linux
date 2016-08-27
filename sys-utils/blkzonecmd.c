/*
 * blkreport.c -- request a zone report on part (or all) of the block device.
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

#ifndef HAVE_BLKZONED_API_H
/**
 * struct bdev_zone_action - ioctl: Perform Zone Action
 *
 * @zone_locator_lba: starting lba for first [reported] zone
 * @return_page_count: number of *bytes* allocated for result
 * @action: One of the ZONE_ACTION_*'s Close,Finish,Open, or Reset
 * @all_zones: Flag to indicate if command should apply to all zones.
 * @force_unit_access: Force command to media (bypass zone cache).
 *
 * Used to issue report zones command to connected device
 */
struct bdev_zone_action {
	__u64 zone_locator_lba;
	__u32 action;
	__u8  all_zones;
	__u8  force_unit_access;
} __attribute__((packed));

#endif /* HAVE_BLKZONED_API_H */

#ifndef BLKZONEACTION
#define BLKZONEACTION	_IOW(0x12, 131, struct bdev_zone_action)

#define ZONE_ACTION_CLOSE	0x01
#define ZONE_ACTION_FINISH	0x02
#define ZONE_ACTION_OPEN	0x03
#define ZONE_ACTION_RESET	0x04

#endif /* BLKZONEACTION */

static void print_stats(int act, char *path, uint64_t lba)
{
	switch (act) {
	case ZONE_ACTION_CLOSE:
		printf(_("%s: Close Zone %" PRIu64 "\n"), path, lba);
		break;
	case ZONE_ACTION_FINISH:
		printf(_("%s: Open Zone %" PRIu64 "\n"), path, lba);
		break;
	case ZONE_ACTION_OPEN:
		printf(_("%s: Open Zone %" PRIu64 "\n"), path, lba);
		break;
	case ZONE_ACTION_RESET:
		printf(_("%s: Reset Zone %" PRIu64 "\n"), path, lba);
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
	fputs(_(" -z, --zone <num>  lba of start of zone to act upon\n"
		" -o, --open        open zone\n"
		" -c, --close       close zone\n"
		" -f, --finish      finish zone\n"
		" -r, --reset       reset zone\n"
		" -a, --all         apply to all zones\n"
		" -F, --force       force command to be set to media\n"
		" -v, --verbose     print aligned length and offset"),
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
	struct bdev_zone_action za;
	uint64_t zone_lba = 0ul;
	uint32_t act = ZONE_ACTION_OPEN;
	int fua = 0;
	int rc = 0;
	int all = 0;

	static const struct option longopts[] = {
	    { "help",      0, 0, 'h' },
	    { "version",   0, 0, 'V' },
	    { "all",       0, 0, 'a' },
	    { "zone",      1, 0, 'z' },
	    { "close",     0, 0, 'c' },
	    { "finish",    0, 0, 'f' },
	    { "force",     0, 0, 'F' },
	    { "open",      0, 0, 'o' },
	    { "reset",     0, 0, 'r' },
	    { "verbose",   0, 0, 'v' },
	    { NULL,        0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "ahVvocFfrz:", longopts, NULL)) != -1) {
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
		case 'o':
			act = ZONE_ACTION_OPEN;
			break;
		case 'c':
			act = ZONE_ACTION_CLOSE;
			break;
		case 'f':
			act = ZONE_ACTION_FINISH;
			break;
		case 'r':
			act = ZONE_ACTION_RESET;
			break;
		case 'a':
			all = 1;
			break;
		case 'F':
			fua = 1;
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

	switch (act) {
	case ZONE_ACTION_CLOSE:
	case ZONE_ACTION_FINISH:
	case ZONE_ACTION_OPEN:
	case ZONE_ACTION_RESET:
		za.zone_locator_lba = zone_lba;
		za.all_zones = all;
		if (zone_lba == ~0ul) {
			za.zone_locator_lba = 0;
			za.all_zones = 1;
		}
		if (za.all_zones && za.zone_locator_lba)
			err(EXIT_FAILURE, _("%s: All expects zone to be 0"), path);
		za.action = act;
		za.force_unit_access = fua;
		rc = ioctl(fd, BLKZONEACTION, &za);
		if (rc == -1)
			err(EXIT_FAILURE, _("%s: BLKZONEACTION ioctl failed"), path);
		break;
	default:
		err(EXIT_FAILURE, _("%s: Unknown zone action %d"), path, act);
		break;
	}

	if (verbose && zone_lba)
		print_stats(act, path, zone_lba);

	close(fd);
	return EXIT_SUCCESS;
}
