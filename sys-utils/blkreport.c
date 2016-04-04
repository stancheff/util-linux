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

#ifndef HAVE_BLKZONED_API_H
/**
 * enum zone_report_option - Report Zones types to be included.
 *
 * @ZOPT_NON_SEQ_AND_RESET: Default (all zones).
 * @ZOPT_ZC1_EMPTY: Zones which are empty.
 * @ZOPT_ZC2_OPEN_IMPLICIT: Zones open but not explicitly opened
 * @ZOPT_ZC3_OPEN_EXPLICIT: Zones opened explicitly
 * @ZOPT_ZC4_CLOSED: Zones closed for writing.
 * @ZOPT_ZC5_FULL: Zones that are full.
 * @ZOPT_ZC6_READ_ONLY: Zones that are read-only
 * @ZOPT_ZC7_OFFLINE: Zones that are offline
 * @ZOPT_RESET: Zones that are empty
 * @ZOPT_NON_SEQ: Zones that have cache writes pending
 * @ZOPT_NON_WP_ZONES: Zones that do not have Write Pointers (conventional)
 *
 * @ZOPT_USE_ATA_PASS: Flag used in kernel to service command I/O
 *
 * Used by Report Zones in bdev_zone_get_report: report_option
 */
enum zone_report_option {
	ZOPT_NON_SEQ_AND_RESET   = 0x00,
	ZOPT_ZC1_EMPTY,
	ZOPT_ZC2_OPEN_IMPLICIT,
	ZOPT_ZC3_OPEN_EXPLICIT,
	ZOPT_ZC4_CLOSED,
	ZOPT_ZC5_FULL,
	ZOPT_ZC6_READ_ONLY,
	ZOPT_ZC7_OFFLINE,
	ZOPT_RESET               = 0x10,
	ZOPT_NON_SEQ             = 0x11,
	ZOPT_NON_WP_ZONES        = 0x3f,
	ZOPT_USE_ATA_PASS        = 0x80,
};

/**
 * enum bdev_zone_type - Type of zone in descriptor
 *
 * @ZTYP_RESERVED: Reserved
 * @ZTYP_CONVENTIONAL: Conventional random write zone (No Write Pointer)
 * @ZTYP_SEQ_WRITE_REQUIRED: Non-sequential writes are rejected.
 * @ZTYP_SEQ_WRITE_PREFERRED: Non-sequential writes allowed but discouraged.
 *
 * Returned from Report Zones. See bdev_zone_descriptor* type.
 */
enum bdev_zone_type {
	ZTYP_RESERVED            = 0,
	ZTYP_CONVENTIONAL        = 1,
	ZTYP_SEQ_WRITE_REQUIRED  = 2,
	ZTYP_SEQ_WRITE_PREFERRED = 3,
};


/**
 * enum bdev_zone_condition - Condition of zone in descriptor
 *
 * @ZCOND_CONVENTIONAL: N/A
 * @ZCOND_ZC1_EMPTY: Empty
 * @ZCOND_ZC2_OPEN_IMPLICIT: Opened via write to zone.
 * @ZCOND_ZC3_OPEN_EXPLICIT: Opened via open zone command.
 * @ZCOND_ZC4_CLOSED: Closed
 * @ZCOND_ZC6_READ_ONLY:
 * @ZCOND_ZC5_FULL: No remaining space in zone.
 * @ZCOND_ZC7_OFFLINE: Offline
 *
 * Returned from Report Zones. See bdev_zone_descriptor* flags.
 */
enum bdev_zone_condition {
	ZCOND_CONVENTIONAL       = 0,
	ZCOND_ZC1_EMPTY          = 1,
	ZCOND_ZC2_OPEN_IMPLICIT  = 2,
	ZCOND_ZC3_OPEN_EXPLICIT  = 3,
	ZCOND_ZC4_CLOSED         = 4,
	/* 0x5 to 0xC are reserved */
	ZCOND_ZC6_READ_ONLY      = 0xd,
	ZCOND_ZC5_FULL           = 0xe,
	ZCOND_ZC7_OFFLINE        = 0xf,
};


/**
 * enum bdev_zone_same - Report Zones same code.
 *
 * @ZS_ALL_DIFFERENT: All zones differ in type and size.
 * @ZS_ALL_SAME: All zones are the same size and type.
 * @ZS_LAST_DIFFERS: All zones are the same size and type except the last zone.
 * @ZS_SAME_LEN_DIFF_TYPES: All zones are the same length but types differ.
 *
 * Returned from Report Zones. See bdev_zone_report* same_field.
 */
enum bdev_zone_same {
	ZS_ALL_DIFFERENT        = 0,
	ZS_ALL_SAME             = 1,
	ZS_LAST_DIFFERS         = 2,
	ZS_SAME_LEN_DIFF_TYPES  = 3,
};


/**
 * struct bdev_zone_get_report - ioctl: Report Zones request
 *
 * @zone_locator_lba: starting lba for first [reported] zone
 * @return_page_count: number of *bytes* allocated for result
 * @report_option: see: zone_report_option enum
 *
 * Used to issue report zones command to connected device
 */
struct bdev_zone_get_report {
	__u64 zone_locator_lba;
	__u32 return_page_count;
	__u8  report_option;
} __attribute__((packed));

/**
 * struct bdev_zone_descriptor_le - See: bdev_zone_descriptor
 */
struct bdev_zone_descriptor_le {
	__u8 type;
	__u8 flags;
	__u8 reserved1[6];
	__le64 length;
	__le64 lba_start;
	__le64 lba_wptr;
	__u8 reserved[32];
} __attribute__((packed));


/**
 * struct bdev_zone_report_le - See: bdev_zone_report
 */
struct bdev_zone_report_le {
	__le32 descriptor_count;
	__u8 same_field;
	__u8 reserved1[3];
	__le64 maximum_lba;
	__u8 reserved2[48];
	struct bdev_zone_descriptor_le descriptors[0];
} __attribute__((packed));


/**
 * struct bdev_zone_descriptor - A Zone descriptor entry from report zones
 *
 * @type: see zone_type enum
 * @flags: Bits 0:reset, 1:non-seq, 2-3: resv, 4-7: see zone_condition enum
 * @reserved1: padding
 * @length: length of zone in sectors
 * @lba_start: lba where the zone starts.
 * @lba_wptr: lba of the current write pointer.
 * @reserved: padding
 *
 */
struct bdev_zone_descriptor {
	__u8 type;
	__u8 flags;
	__u8  reserved1[6];
	__be64 length;
	__be64 lba_start;
	__be64 lba_wptr;
	__u8 reserved[32];
} __attribute__((packed));


/**
 * struct bdev_zone_report - Report Zones result
 *
 * @descriptor_count: Number of descriptor entries that follow
 * @same_field: bits 0-3: enum zone_same (MASK: 0x0F)
 * @reserved1: padding
 * @maximum_lba: LBA of the last logical sector on the device, inclusive
 *               of all logical sectors in all zones.
 * @reserved2: padding
 * @descriptors: array of descriptors follows.
 */
struct bdev_zone_report {
	__be32 descriptor_count;
	__u8 same_field;
	__u8 reserved1[3];
	__be64 maximum_lba;
	__u8 reserved2[48];
	struct bdev_zone_descriptor descriptors[0];
} __attribute__((packed));


/**
 * struct bdev_zone_report_io - Report Zones ioctl argument.
 *
 * @in: Report Zones inputs
 * @out: Report Zones output
 */
struct bdev_zone_report_io {
	union {
		struct bdev_zone_get_report in;
		struct bdev_zone_report out;
	} data;
} __attribute__((packed));

#endif

#ifndef BLKREPORT
# define BLKREPORT	_IOWR(0x12, 130, struct bdev_zone_report_io)
#endif

static const char * same_text[] = {
	"all zones are different",
	"all zones are same size",
	"last zone differs by size",
	"all zones same size - different types",
};

static const char * type_text[] = {
	"RESERVED",
	"CONVENTIONAL",
	"SEQ_WRITE_REQUIRED",
	"SEQ_WRITE_PREFERRED",
};

#define ARRAY_COUNT(x) (sizeof((x))/sizeof((*x)))

int fix_endian = 0;

static inline uint64_t endian64(uint64_t in)
{
	return fix_endian ? be64toh(in) : in;
}

static inline uint32_t endian32(uint32_t in)
{
	return fix_endian ? be32toh(in) : in;
}

static inline uint16_t endian16(uint16_t in)
{
	return fix_endian ? be16toh(in) : in;
}

static void test_endian(struct bdev_zone_report * info)
{
	struct bdev_zone_descriptor * entry = &info->descriptors[0];
	uint64_t be_len;
	be_len = be64toh(entry->length);
	if ( be_len == 0x080000 ||
             be_len == 0x100000 ||
             be_len == 0x200000 ||
             be_len == 0x300000 ||
             be_len == 0x400000 ||
             be_len == 0x800000 ) {
		fprintf(stdout, "*** RESULTS are BIG ENDIAN ****\n");
		fix_endian = 1;
	} else {
		fprintf(stdout, "*** RESULTS are LITTLE ENDIAN ****\n");
	}
}

const char * condition_str[] = {
	"cv", /* conventional zone */
	"e0", /* empty */
	"Oi", /* open implicit */
	"Oe", /* open explicit */
	"Cl", /* closed */
	"x5", "x6", "x7", "x8", "x9", "xA", "xB", /* xN: reserved */
	"ro", /* read only */
	"fu", /* full */
	"OL"  /* offline */
	};

static const char * zone_condition_str(uint8_t cond)
{
	return condition_str[cond & 0x0f];
}

static void print_zones(struct bdev_zone_report * info, uint32_t size)
{
	uint32_t count = endian32(info->descriptor_count);
	uint32_t max_count;
	uint32_t iter;
	int same_code = info->same_field & 0x0f;

	fprintf(stdout, "  count: %u, same %u (%s), max_lba %lu\n",
		count,
		same_code, same_text[same_code],
		endian64(info->maximum_lba & (~0ul >> 16)) );

	max_count = (size - sizeof(struct bdev_zone_report))
                        / sizeof(struct bdev_zone_descriptor);
	if (count > max_count) {
		fprintf(stdout, "Truncating report to %d of %d zones.\n",
			max_count, count );
		count = max_count;
	}

	for (iter = 0; iter < count; iter++ ) {
		struct bdev_zone_descriptor * entry = &info->descriptors[iter];
		unsigned int type  = entry->type & 0xF;
		unsigned int flags = entry->flags;
		uint64_t start = endian64(entry->lba_start);
		uint64_t wp = endian64(entry->lba_wptr);
		uint8_t cond = (flags & 0xF0) >> 4;
		uint64_t len = endian64(entry->length);

		if (!len) {
			break;
		}
		fprintf(stdout,
			"  start: %9lx, len %7lx, wptr %8lx"
			" reset:%u non-seq:%u, zcond:%2u(%s) [type: %u(%s)]\n",
		start, len, wp - start, flags & 0x01, (flags & 0x02) >> 1,
		cond, zone_condition_str(cond), type, type_text[type]);
	}
}

static inline int is_report_option_valid(uint64_t ropt)
{
	return (ropt <= ZOPT_ZC7_OFFLINE || ropt == ZOPT_RESET ||
		ropt == ZOPT_NON_SEQ || ropt == ZOPT_NON_WP_ZONES);
}

static int do_report(int fd, uint64_t lba, uint64_t len, uint8_t ropt, int do_ata, int verbose)
{
	int rc = -4;
	struct bdev_zone_report_io * zone_info;

	zone_info = malloc(len);
	if (zone_info) {
		memset(zone_info, 0, len);
		zone_info->data.in.report_option     = ropt;
		zone_info->data.in.return_page_count = len;
		zone_info->data.in.zone_locator_lba  = lba;

		if (do_ata) {
			zone_info->data.in.report_option |= 0x80;
		}

		rc = ioctl(fd, BLKREPORT, zone_info);
		if (rc != -1) {
			test_endian(&zone_info->data.out);

			if (verbose)
				fprintf(stdout, "Found %d zones\n",
					endian32(zone_info->data.out.descriptor_count));

			print_zones(&zone_info->data.out, len);
		} else {
			fprintf(stderr, "ERR: %d -> %s\n\n", errno, strerror(errno));
		}
	}

	return rc;
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Discard the content of sectors on a device.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -z, --zone <num>  zone lba in bytes to report from\n"
		" -l, --length <num>  length of report (512 bytes to 512k bytes)\n"
		" -r, --option <report> report option\n"
		"    report is the numeric value from \"enum zone_report_option\".\n"
		"             0 - non seq. and reset (default)\n"
		"             1 - empty\n"
		"             2 - open implicit\n"
		"             3 - open explicit\n"
		"             4 - closed\n"
		"             5 - full\n"
		"             6 - read only\n"
		"             7 - offline\n"
		"          0x10 - reset\n"
		"          0x11 - non sequential\n"
		"          0x3f - non write pointer zones\n"
		" -a, --ata use ATA passthrough to workaround FW in old SAS HBAs\n"
		" -v, --verbose       print aligned length and offset\n"),
		out);
	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("blkreport(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}


#define MAX_REPORT_LEN (1 << 19) /* 512k */

int main(int argc, char **argv)
{
	char *path;
	int c;
	int fd;
	int secsize;
	uint64_t blksize;
	struct stat sb;
	int verbose = 0;
	uint64_t ropt = ZOPT_NON_SEQ_AND_RESET;
	uint64_t offset = 0ul;
	uint32_t length = MAX_REPORT_LEN;
	int ata = 0;

	static const struct option longopts[] = {
	    { "help",      0, 0, 'h' },
	    { "version",   0, 0, 'V' },
	    { "zone",      1, 0, 'z' }, /* starting LBA */
	    { "length",    1, 0, 'l' }, /* max #of bytes for result */
	    { "option",    1, 0, 'r' }, /* report option */
	    { "ata",       0, 0, 'a' }, /* use ATA passthrough */
	    { "verbose",   0, 0, 'v' },
	    { NULL,        0, 0, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "ahVsvz:l:r:", longopts, NULL)) != -1) {
		switch(c) {
		case 'h':
			usage(stdout);
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'l':
			length = strtou64_base_or_err(0, optarg,
					_("failed to parse length"));
			break;
		case 'z':
			offset = strtou64_base_or_err(0, optarg,
					_("failed to parse offset"));
			break;
		case 'r':
			ropt = strtou64_base_or_err(0, optarg,
					_("failed to parse report option"));
			break;
		case 'a':
			ata = 1;
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

	fd = open(path, O_RDWR);
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

	/* check offset alignment to the sector size */
	if (offset % secsize)
		errx(EXIT_FAILURE, _("%s: offset %" PRIu64 " is not aligned "
			 "to sector size %i"), path, offset, secsize);

	/* is the range end behind the end of the device ?*/
	if (offset > blksize)
		errx(EXIT_FAILURE, _("%s: offset is greater than device size"), path);

	length = (length / 512) * 512;
	if (length < 512)
		length = 512;
	if (length > MAX_REPORT_LEN)
		length = MAX_REPORT_LEN;

	if (!is_report_option_valid(ropt))
		errx(EXIT_FAILURE, _("%s: invalid report option for device"), path);

	if (do_report(fd, offset, length, ropt & 0xFF, ata, verbose))
		 err(EXIT_FAILURE, _("%s: BLKREPORT ioctl failed"), path);

	close(fd);
	return EXIT_SUCCESS;
}
