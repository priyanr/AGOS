#ifndef DRIVERS_BLOCK_H
#define DRIVERS_BLOCK_H

#include <kernel/types.h>

typedef struct block_device {
    char name[32];
    uint64_t sector_count;
    uint32_t sector_size;
    int (*read_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer);
    int (*write_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
    void *priv;
    struct block_device *next;
} block_device_t;

void block_register_device(block_device_t *dev);
void block_unregister_device(block_device_t *dev);
block_device_t *block_get_device(const char *name);
block_device_t *block_get_first_device(void);
void ata_init(void);

#endif // DRIVERS_BLOCK_H
