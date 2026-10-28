#include <limine.h>
#include <kernel/types.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/fb.h>
#include <hal/cpu.h>
#include <hal/serial.h>
#include <hal/gdt.h>
#include <hal/idt.h>
#include <hal/pic.h>
#include <hal/apic.h>
#include <hal/timer.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <drivers/keyboard.h>
#include <drivers/pci.h>
#include <drivers/block.h>
#include <drivers/ahci.h>
#include <hal/platform.h>

// Limine Boot Protocol Requests
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request exec_file_req = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void *limine_get_kernel_file(size_t *size_out) {
    if (exec_file_req.response && exec_file_req.response->executable_file) {
        if (size_out) *size_out = exec_file_req.response->executable_file->size;
        return exec_file_req.response->executable_file->address;
    }
    return NULL;
}

void *limine_get_module(const char *name_sub, size_t *size_out) {
    if (module_req.response) {
        for (uint64_t i = 0; i < module_req.response->module_count; i++) {
            struct limine_file *f = module_req.response->modules[i];
            if (f && f->path && strstr(f->path, name_sub)) {
                if (size_out) *size_out = f->size;
                return f->address;
            }
        }
        if (module_req.response->module_count > 0 && !name_sub) {
            struct limine_file *f = module_req.response->modules[0];
            if (size_out) *size_out = f->size;
            return f->address;
        }
    }
    return NULL;
}

// Limine entry point: x86 HAL, memory and PC drivers, then the shared kernel start-up
void kmain(void) {
    // 1. Early Debugging Output (UART 16550 Serial)
    serial_init();
    serial_write("\n=========================================\n");
    serial_write("   Antigravity OS (x86_64) Booting...    \n");
    serial_write("=========================================\n");

    // 2. Linear Framebuffer Graphics
    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
        fb_init((uint64_t)fb->address, fb->width, fb->height, fb->pitch, fb->bpp);
        printk("[OK] Framebuffer initialized: %llux%llu (%u bpp)\n",
               fb->width, fb->height, fb->bpp);
    } else {
        serial_write("[WARN] No framebuffer provided by bootloader.\n");
    }

    // 3. Hardware Abstraction Layer (HAL): CPU Tables
    gdt_init();
    printk("[OK] GDT & TSS loaded.\n");

    idt_init();
    printk("[OK] IDT loaded with 256 vector gates.\n");

    pic_remap(0x20, 0x28);
    printk("[OK] 8259 PIC remapped (Master 0x20, Slave 0x28).\n");

    // 4. Memory Management (PMM, VMM, Heap)
    if (!memmap_req.response || !hhdm_req.response) {
        printk("FATAL: Limine memmap or HHDM request failed!\n");
        while (1) hlt();
    }

    uint64_t hhdm_offset = hhdm_req.response->offset;
    struct limine_memmap_response *memmap = memmap_req.response;
    static pmm_region_t regions[64];
    size_t region_count = 0;
    for (uint64_t i = 0; i < memmap->entry_count && region_count < 64; i++) {
        if (memmap->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            regions[region_count].base = memmap->entries[i]->base;
            regions[region_count].length = memmap->entries[i]->length;
            region_count++;
        }
    }
    pmm_init(regions, region_count, hhdm_offset);
    vmm_init(hhdm_offset);
    heap_init();
    apic_init();

    // 5. Timer & Preemption Hardware
    timer_init(1000); // 1000 Hz = 1ms per tick
    printk("[OK] PIT Timer running at 1000 Hz (1ms tick).\n");

    // 6. Input Drivers
    keyboard_init();
    printk("[OK] PS/2 Keyboard driver active.\n");

    // 7. Bus & Device Drivers
    pci_init();
    ata_init();
    ahci_init();

    kernel_start();
}
