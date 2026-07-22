/* SPDX-FileCopyrightText: 2025 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "zealfs_v2.h"

/**
 * @brief Look for a ZealFS partition in the image file. If it has an MBR, it will check the 4 partitions inside,
 * If it's a raw image, it will return sector 0 and the image size.
 */
#define MBR_SIZE                512
#define PARTITION_ENTRY_SIZE    16
#define PARTITION_TABLE_OFFSET  446
#define PARTITION_TYPE_OFFSET   4
#define LBA_OFFSET              8
#define SECTOR_COUNT_OFFSET     12
#define SECTOR_SIZE             512

#define PARTITION_STATUS_OFFSET 0
#define MAX_PRIMARY_PARTITIONS 4

int mbr_create(uint8_t* out_mbr, off_t part_offset, size_t part_size)
{
    /* The size should already be a multiple of 512 (since it's a multiple of KB), but let's
     * be safe and test it again. */
    if (part_offset % SECTOR_SIZE != 0 || part_size % SECTOR_SIZE != 0) {
        printf("ERROR: Partition offset and size must be a multiple of %d!\n", SECTOR_SIZE);
        return 1;
    }
    memset(out_mbr, 0, MBR_SIZE);

    /* Build partition entry in the MBR */
    uint8_t* entry = out_mbr + PARTITION_TABLE_OFFSET;
    entry[PARTITION_STATUS_OFFSET] = 0x00;          /* Not bootable */
    entry[PARTITION_TYPE_OFFSET]   = TARGET_TYPE;   /* Your custom FS type */
    /* Partition type */
    entry[PARTITION_TYPE_OFFSET] = TARGET_TYPE;
    /* Ending CHS = zero */
    entry[5] = 0x00;
    entry[6] = 0x00;
    entry[7] = 0x00;
    /* Write LBA (little endian) */
    const uint32_t lba = (uint32_t)(part_offset / SECTOR_SIZE);
    entry[LBA_OFFSET + 0] = lba & 0xFF;
    entry[LBA_OFFSET + 1] = (lba >> 8) & 0xFF;
    entry[LBA_OFFSET + 2] = (lba >> 16) & 0xFF;
    entry[LBA_OFFSET + 3] = (lba >> 24) & 0xFF;
    /* Write sector count (little endian) */
    const uint32_t sectors = (uint32_t)(part_size / SECTOR_SIZE);
    entry[SECTOR_COUNT_OFFSET + 0] = sectors & 0xFF;
    entry[SECTOR_COUNT_OFFSET + 1] = (sectors >> 8) & 0xFF;
    entry[SECTOR_COUNT_OFFSET + 2] = (sectors >> 16) & 0xFF;
    entry[SECTOR_COUNT_OFFSET + 3] = (sectors >> 24) & 0xFF;
    /* Append the MBR signature */
    out_mbr[510] = 0x55;
    out_mbr[511] = 0xAA;
    return 0;
}


void mbr_list_partitions(const char* filename, int filesize)
{
    struct { 
        int entry;
        uint32_t lba;
        uint32_t count;
    } zparts[MAX_PRIMARY_PARTITIONS];
    uint8_t mbr[MBR_SIZE];

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Could not open file");
        return;
    }

    ssize_t bytes_read = read(fd, mbr, MBR_SIZE);
    if (bytes_read != MBR_SIZE) {
        perror("Failed to read MBR");
        close(fd);
        return;
    }

    /* Check for MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        /* No MBR -- check if it's a raw ZealFS image */
        if (mbr[0] == TARGET_TYPE) {
            printf("Raw ZealFS image (no MBR), single partition at index 0, size %d bytes\n",
                   filesize);
        } else {
            printf("No valid MBR signature found\n");
        }
        close(fd);
        return;
    }

    /* Collect all ZealFS partitions into a small array (at most 4 entries) */
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE;
        if (e[PARTITION_TYPE_OFFSET] == TARGET_TYPE) {
            zparts[n].entry = i;
            zparts[n].lba   = *(const uint32_t *)(e + LBA_OFFSET);
            zparts[n].count = *(const uint32_t *)(e + SECTOR_COUNT_OFFSET);
            n++;
        }
    }

    if (n == 0) {
        printf("MBR present but no ZealFS partition found.\n");
        close(fd);
        return;
    }

    printf("\nZealFS partitions in: %s\n", filename);
    printf("  Index  Entry  LBA (sectors)  Size (sectors)  Size\n");
    printf("  -----  -----  ------------  --------------  ----------\n");
    for (int i = 0; i < n; i++) {
        printf("     %d     %d  %11u  %14u  %s  %u bytes\n",
            i, zparts[i].entry, zparts[i].lba, zparts[i].count,
            zparts[i].count >= 2048 ? "  " : "     ",
            zparts[i].count * SECTOR_SIZE);
    }
    printf("\n");
    printf("Use --partition=N (0-based) to select a ZealFS partition.\n");
    printf("Non-ZealFS partitions are not shown.\n\n");

    close(fd);
}


int mbr_find_partition(const char* filename, int filesize, off_t* offset, int* size, int partition_index)
{
    uint8_t mbr[MBR_SIZE];

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Could not open file");
        return 0;
    }

    ssize_t bytes_read = read(fd, mbr, MBR_SIZE);
    if (bytes_read != MBR_SIZE) {
        perror("Failed to read MBR");
        close(fd);
        return 0;
    }

    /* Check for MBR signature (0x55AA at offset 510) */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        /* Invalid MBR signature, check if it's a raw ZealFS image */
        close(fd);
        if (mbr[0] == TARGET_TYPE) {
            if (partition_index == 0) {
                *offset = 0;
                *size = filesize;
                return 1;
            }
            printf("ERROR: Partition index %d requested but only 1 ZealFS partition exists "
                   "(raw image, no MBR)\n", partition_index);
        }
        return 0;
    }

    int zealfs_count = 0;

    /* Iterate through the primary partition entries */
    for (int i = 0; i < MAX_PRIMARY_PARTITIONS; i++) {
        uint8_t *entry = mbr + PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE;
        uint8_t type = entry[PARTITION_TYPE_OFFSET];

        if (type == 0) {
            continue;  /* Unused entry */
        }

        if (type == TARGET_TYPE) {
            if (zealfs_count == partition_index) {
                const uint32_t lba = *(uint32_t *)(entry + LBA_OFFSET);
                const uint32_t sector_count = *(uint32_t *)(entry + SECTOR_COUNT_OFFSET);
                *offset = (off_t)lba * SECTOR_SIZE;
                *size = sector_count * SECTOR_SIZE;
                close(fd);
                return 1;
            }
            zealfs_count++;
        }
    }

    close(fd);

    if (zealfs_count > 0) {
        printf("ERROR: Partition index %d requested but only %d ZealFS partition(s) found "
               "(0-based index)\n", partition_index, zealfs_count);
    }

    return 0;
}

