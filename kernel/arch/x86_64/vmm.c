#include <mm/vmm.h>
#include <mm/pmm.h>
#include <hal/cpu.h>
#include <kernel/string.h>
#include <kernel/printk.h>

static address_space_t kernel_as;
static uint64_t hhdm_base = 0;

uint64_t vmm_get_hhdm(void) {
    return hhdm_base;
}

void *vmm_phys_to_virt(paddr_t phys) {
    return (void *)(phys + hhdm_base);
}

static inline paddr_t virt_to_phys_direct(void *virt) {
    return (paddr_t)((uintptr_t)virt - hhdm_base);
}

static inline uint64_t pml4_index(vaddr_t vaddr) {
    return (vaddr >> 39) & 0x1FF;
}

static inline uint64_t pdpt_index(vaddr_t vaddr) {
    return (vaddr >> 30) & 0x1FF;
}

static inline uint64_t pd_index(vaddr_t vaddr) {
    return (vaddr >> 21) & 0x1FF;
}

static inline uint64_t pt_index(vaddr_t vaddr) {
    return (vaddr >> 12) & 0x1FF;
}

static page_table_t get_next_level(page_table_t current, uint64_t index, bool allocate, uint64_t flags) {
    if (current[index] & PTE_PRESENT) {
        paddr_t phys = current[index] & PTE_ADDR_MASK;
        return (page_table_t)vmm_phys_to_virt(phys);
    }

    if (!allocate) return NULL;

    paddr_t new_table_phys = pmm_alloc_page();
    if (!new_table_phys) {
        printk("VMM: Out of memory while allocating page table level!\n");
        return NULL;
    }

    page_table_t new_table_virt = (page_table_t)vmm_phys_to_virt(new_table_phys);
    memset(new_table_virt, 0, PAGE_SIZE);

    current[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return new_table_virt;
}

bool vmm_map_page(page_table_t pml4, vaddr_t vaddr, paddr_t paddr, uint64_t flags) {
    if (!pml4) return false;

    page_table_t pdpt = get_next_level(pml4, pml4_index(vaddr), true, flags);
    if (!pdpt) return false;

    page_table_t pd = get_next_level(pdpt, pdpt_index(vaddr), true, flags);
    if (!pd) return false;

    page_table_t pt = get_next_level(pd, pd_index(vaddr), true, flags);
    if (!pt) return false;

    uint64_t idx = pt_index(vaddr);
    pt[idx] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    invlpg(vaddr);
    return true;
}

void vmm_unmap_page(page_table_t pml4, vaddr_t vaddr) {
    if (!pml4) return;

    page_table_t pdpt = get_next_level(pml4, pml4_index(vaddr), false, 0);
    if (!pdpt) return;

    page_table_t pd = get_next_level(pdpt, pdpt_index(vaddr), false, 0);
    if (!pd) return;

    page_table_t pt = get_next_level(pd, pd_index(vaddr), false, 0);
    if (!pt) return;

    uint64_t idx = pt_index(vaddr);
    pt[idx] = 0;

    invlpg(vaddr);
}

paddr_t vmm_virt_to_phys(page_table_t pml4, vaddr_t vaddr) {
    if (!pml4) return 0;

    page_table_t pdpt = get_next_level(pml4, pml4_index(vaddr), false, 0);
    if (!pdpt) return 0;

    page_table_t pd = get_next_level(pdpt, pdpt_index(vaddr), false, 0);
    if (!pd) return 0;

    page_table_t pt = get_next_level(pd, pd_index(vaddr), false, 0);
    if (!pt) return 0;

    uint64_t pte = pt[pt_index(vaddr)];
    if (!(pte & PTE_PRESENT)) return 0;

    return (pte & PTE_ADDR_MASK) | (vaddr & 0xFFF);
}

address_space_t vmm_create_address_space(void) {
    address_space_t as;
    as.pml4_phys = pmm_alloc_page();
    as.pml4_virt = (page_table_t)vmm_phys_to_virt(as.pml4_phys);

    memset(as.pml4_virt, 0, PAGE_SIZE);

    // Share upper-half kernel space (entries 256..511)
    for (int i = 256; i < 512; i++) {
        as.pml4_virt[i] = kernel_as.pml4_virt[i];
    }

    return as;
}

void vmm_destroy_address_space(address_space_t *as) {
    if (!as || !as->pml4_phys) return;
    // Free user space page tables (entries 0..255)
    // To be expanded with full recursive tear-down
    pmm_free_page(as->pml4_phys);
    as->pml4_phys = 0;
    as->pml4_virt = NULL;
}

void vmm_switch_address_space(address_space_t *as) {
    if (!as || !as->pml4_phys) return;
    write_cr3(as->pml4_phys);
}

address_space_t *vmm_get_kernel_address_space(void) {
    return &kernel_as;
}

void vmm_page_fault_handler(interrupt_frame_t *frame) {
    uint64_t fault_addr = read_cr2();
    bool present = (frame->err_code & (1 << 0)) != 0;
    bool write = (frame->err_code & (1 << 1)) != 0;
    bool user = (frame->err_code & (1 << 2)) != 0;
    bool instruction_fetch = (frame->err_code & (1 << 4)) != 0;

    printk("\n!!! PAGE FAULT (#PF) Vector 14 !!!\n");
    printk("Address: 0x%016llx  RIP: 0x%016llx\n", fault_addr, frame->rip);
    printk("Reason:  %s, %s, %s%s\n",
           present ? "Page-protection violation" : "Non-present page",
           write ? "Write" : "Read",
           user ? "User-mode" : "Kernel-mode",
           instruction_fetch ? ", Instruction fetch" : "");
    printk("System halted.\n");
    cli();
    while (1) {
        hlt();
    }
}

void vmm_init(uint64_t hhdm_offset) {
    hhdm_base = hhdm_offset;

    // Use current PML4 created by Limine or clone it
    kernel_as.pml4_phys = read_cr3();
    kernel_as.pml4_virt = (page_table_t)vmm_phys_to_virt(kernel_as.pml4_phys);

    // Register Page Fault (#PF) handler Vector 14
    idt_register_handler(14, vmm_page_fault_handler);

    printk("VMM: Initialized. Kernel PML4 at Phys 0x%016llx (Virt 0x%016llx)\n",
           kernel_as.pml4_phys, (uint64_t)kernel_as.pml4_virt);
}
