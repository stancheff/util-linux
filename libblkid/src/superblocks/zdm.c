/*
 * Copyright (C) 2015 Shaun Tancheff <shaun@tancheff.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 *
 * Based on code fragments from zdm-tools
 */

#include <stddef.h>
#include <stdio.h>
#include <uuid/uuid.h>

#include "superblocks.h"
#include "crc32.h"

static const char zdm_magic[] = {
	0x7a, 0x6f, 0x6e, 0x65, 0x63, 0x44, 0x45, 0x56,
	0x82, 0x65, 0xf5, 0x7f, 0x48, 0xba, 0x6d, 0x81
};

struct zdm_super_block {
	uint32_t crc32;
	uint32_t reserved;
	uint8_t magic[ARRAY_SIZE(zdm_magic)];
	uuid_t  uuid;
	uint32_t version;     /* 0xMMMMmmpt */
	uint64_t sect_start;
	uint64_t sect_size;
	uint32_t mz_metadata_zones;     /* 3 (default) */
	uint32_t mz_over_provision;     /* 5 (minimum) */
	uint64_t zdm_blocks;  /* 0 -> <zdm_blocks> for dmsetup table entry */
	uint32_t discard;     /* if discard support is enabled */
	uint32_t disk_type;   /* HA | HM */
	uint32_t zac_zbc;     /* if ZAC / ZBC is supported on backing device */
	char label[64];
	uint64_t data_start;  /* zone # of first *DATA* zone */
	uint64_t zone_size;   /* zone size in 512 byte blocks */
};
typedef struct zdm_super_block zdm_super_block_t;

static uint32_t zdm_crc32(zdm_super_block_t *sblk)
{
	uint32_t icrc = sblk->crc32;
	uint8_t *data = (uint8_t *) sblk;
	size_t sz = sizeof(*sblk);
	uint32_t calc;

	sblk->crc32 = 0u;
	calc = crc32(~0u, data, sz) ^ ~0u;
	sblk->crc32 = icrc;

	return calc;
}


static int probe_zdm(blkid_probe pr, const struct blkid_idmag *mag)
{
	zdm_super_block_t *sblk;

	sblk = blkid_probe_get_sb(pr, mag, struct zdm_super_block);
	if (!sblk)
		return errno ? -errno : BLKID_PROBE_NONE;

	if (!blkid_probe_verify_csum(pr, zdm_crc32(sblk), le32_to_cpu(sblk->crc32)))
		return BLKID_PROBE_NONE;

	if (blkid_probe_set_uuid(pr, sblk->uuid) < 0)
		return BLKID_PROBE_NONE;

	return BLKID_PROBE_OK;
}

const struct blkid_idinfo zdm_idinfo =
{
	.name		= "zdm",
	.usage		= BLKID_USAGE_OTHER,
	.probefunc	= probe_zdm,
	.minsz		= 1 << 12,
	.magics		=
	{
		{
		  .magic = zdm_magic,
		  .len   = sizeof(zdm_magic),
		  .kboff = 0,
		  .sboff = offsetof(struct zdm_super_block, magic)
		} ,
		{ NULL }
	}
};
