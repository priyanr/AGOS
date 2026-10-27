#include <hal/apic.h>
#include <hal/cpu.h>
#include <mm/vmm.h>
#include <kernel/printk.h>

#define IA32_APIC_BASE_MSR        0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

#define LAPIC_TPR   0x080
#define LAPIC_EOI   0x0B0
#define LAPIC_SVR   0x0F0
#define LAPIC_LINT0 0x350
#define LAPIC_LINT1 0x360

static volatile uint32_t *lapic_base = NULL;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    if (!lapic_base) return;
    *(volatile uint32_t *)((uintptr_t)lapic_base + reg) = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    if (!lapic_base) return 0;
    return *(volatile uint32_t *)((uintptr_t)lapic_base + reg);
}

void apic_init(void) {
    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    paddr_t apic_phys = apic_msr & 0xFFFFF000;

    if (!apic_phys) {
        apic_phys = 0xFEE00000;
    }

    // Ensure APIC global enable bit is set in MSR
    if (!(apic_msr & IA32_APIC_BASE_MSR_ENABLE)) {
        wrmsr(IA32_APIC_BASE_MSR, apic_msr | IA32_APIC_BASE_MSR_ENABLE);
    }

    // Map the Local APIC page in VMM
    lapic_base = (volatile uint32_t *)vmm_phys_to_virt(apic_phys);
    address_space_t *kas = vmm_get_kernel_address_space();
    if (kas && kas->pml4_virt) {
        vmm_map_page(kas->pml4_virt, (vaddr_t)lapic_base, apic_phys, PTE_WRITABLE);
    }

    // 1. Set Task Priority to 0 (accept all interrupt priorities)
    lapic_write(LAPIC_TPR, 0x00);

    // 2. Enable APIC in Spurious Interrupt Vector Register (offset 0x0F0)
    // Bit 8 = Software Enable (0x100), Vector = 0xFF
    lapic_write(LAPIC_SVR, 0x1FF);

    // 3. Configure LINT0 (offset 0x350) for ExtINT mode:
    // Delivery Mode = 0b111 (0x700), Mask = 0 (unmasked)
    // This routes the 8259 PIC signals directly through the Local APIC to the CPU!
    lapic_write(LAPIC_LINT0, 0x00000700);

    // 4. Configure LINT1 (offset 0x360) for NMI:
    // Delivery Mode = 0b100 (0x400), Mask = 0
    lapic_write(LAPIC_LINT1, 0x00000400);

    printk("[OK] Local APIC initialized (LINT0 ExtINT routed for 8259 PIC).\n");
}
