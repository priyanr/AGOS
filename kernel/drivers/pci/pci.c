#include <drivers/pci.h>
#include <hal/cpu.h>
#include <mm/heap.h>
#include <kernel/printk.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t *pci_device_list = NULL;

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = ((uint32_t)1 << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = ((uint32_t)1 << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void check_device(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t reg0 = pci_read_config32(bus, device, function, 0);
    uint16_t vendor_id = reg0 & 0xFFFF;
    uint16_t device_id = (reg0 >> 16) & 0xFFFF;

    if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
        return; // No device present
    }

    uint32_t reg8 = pci_read_config32(bus, device, function, 8);
    uint8_t class_code = (reg8 >> 24) & 0xFF;
    uint8_t subclass = (reg8 >> 16) & 0xFF;
    uint8_t prog_if = (reg8 >> 8) & 0xFF;

    pci_device_t *dev = kzalloc(sizeof(pci_device_t));
    if (!dev) return;

    dev->bus = bus;
    dev->device = device;
    dev->function = function;
    dev->vendor_id = vendor_id;
    dev->device_id = device_id;
    dev->class_code = class_code;
    dev->subclass = subclass;
    dev->prog_if = prog_if;

    for (int bar = 0; bar < 6; bar++) {
        dev->bar[bar] = pci_read_config32(bus, device, function, 0x10 + (bar * 4));
    }

    dev->next = pci_device_list;
    pci_device_list = dev;
}

void pci_init(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint32_t reg0 = pci_read_config32((uint8_t)bus, device, 0, 0);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;

            uint32_t regC = pci_read_config32((uint8_t)bus, device, 0, 0x0C);
            uint8_t header_type = (regC >> 16) & 0xFF;

            check_device((uint8_t)bus, device, 0);

            if (header_type & 0x80) { // Multi-function device
                for (uint8_t function = 1; function < 8; function++) {
                    check_device((uint8_t)bus, device, function);
                }
            }
        }
    }
    printk("PCI: Bus scan complete.\n");
}

void pci_dump_devices(void) {
    printk("--- PCI Devices Detected ---\n");
    pci_device_t *dev = pci_device_list;
    while (dev) {
        printk("[%02x:%02x.%x] %04x:%04x Class %02x:%02x (Prog-IF %02x)\n",
               dev->bus, dev->device, dev->function,
               dev->vendor_id, dev->device_id,
               dev->class_code, dev->subclass, dev->prog_if);
        dev = dev->next;
    }
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    pci_device_t *dev = pci_device_list;
    while (dev) {
        if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    pci_device_t *dev = pci_device_list;
    while (dev) {
        if (dev->class_code == class_code && dev->subclass == subclass) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}
