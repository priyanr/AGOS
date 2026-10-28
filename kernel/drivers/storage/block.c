#include <drivers/block.h>
#include <kernel/string.h>

// Registry of every disk and partition the drivers found (ATA/AHCI on x86, the SD card on the Pi)
static block_device_t *block_devices = NULL;

void block_register_device(block_device_t *dev) {
    if (!dev) return;
    // Avoid double register
    block_device_t *curr = block_devices;
    while (curr) {
        if (curr == dev) return;
        curr = curr->next;
    }
    dev->next = block_devices;
    block_devices = dev;
}

void block_unregister_device(block_device_t *dev) {
    if (!dev || !block_devices) return;
    if (block_devices == dev) {
        block_devices = dev->next;
        return;
    }
    block_device_t *curr = block_devices;
    while (curr->next) {
        if (curr->next == dev) {
            curr->next = dev->next;
            return;
        }
        curr = curr->next;
    }
}

block_device_t *block_get_device(const char *name) {
    if (!name) return NULL;
    block_device_t *dev = block_devices;
    while (dev) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

block_device_t *block_get_first_device(void) {
    return block_devices;
}
