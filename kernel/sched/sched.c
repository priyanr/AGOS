#include <sched/sched.h>
#include <mm/heap.h>
#include <hal/gdt.h>
#include <hal/cpu.h>
#include <kernel/string.h>
#include <kernel/printk.h>

#define DEFAULT_STACK_SIZE (64 * 1024) // 64 KB kernel stack
#define DEFAULT_TIME_SLICE 20          // 20 timer ticks (20ms at 1000 Hz)

static thread_t *current_thread = NULL;
static thread_t *run_queue = NULL;
static process_t *kernel_process = NULL;
static uint64_t next_tid = 1;
static uint64_t next_pid = 1;
static bool scheduler_enabled = false;

extern void hal_switch_context(uint64_t **old_rsp, uint64_t *new_rsp);
extern void thread_startup(void);

static void queue_thread(thread_t *t) {
    t->state = THREAD_READY;
    if (!run_queue) {
        run_queue = t;
        t->next = t;
        t->prev = t;
    } else {
        thread_t *last = run_queue->prev;
        last->next = t;
        t->prev = last;
        t->next = run_queue;
        run_queue->prev = t;
    }
}

static void dequeue_thread(thread_t *t) {
    if (t->next == t) {
        run_queue = NULL;
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (run_queue == t) {
            run_queue = t->next;
        }
    }
    t->next = NULL;
    t->prev = NULL;
}

process_t *process_create(const char *name) {
    process_t *proc = kzalloc(sizeof(process_t));
    if (!proc) return NULL;

    proc->pid = next_pid++;
    strncpy(proc->name, name ? name : "process", sizeof(proc->name) - 1);
    proc->address_space = vmm_create_address_space();
    return proc;
}

thread_t *thread_create(process_t *proc, const char *name, void (*entry)(void *), void *arg) {
    thread_t *t = kzalloc(sizeof(thread_t));
    if (!t) return NULL;

    t->tid = next_tid++;
    strncpy(t->name, name ? name : "thread", sizeof(t->name) - 1);
    t->stack_size = DEFAULT_STACK_SIZE;
    t->stack_base = kmalloc(t->stack_size);
    if (!t->stack_base) {
        kfree(t);
        return NULL;
    }

    t->time_slice = DEFAULT_TIME_SLICE;
    t->remaining_ticks = t->time_slice;
    t->process = proc ? proc : kernel_process;

    // Set up stack for hal_switch_context:
    // When hal_switch_context switches to this thread:
    // It pops r15, r14, r13, r12, rbp, rbx, rflags and executes ret (jumping to thread_startup).
    uint64_t *stack_top = (uint64_t *)((uintptr_t)t->stack_base + t->stack_size);

    *(--stack_top) = (uint64_t)thread_startup;  // Return address for ret in hal_switch_context
    *(--stack_top) = 0x202;                     // rflags (IF=1)
    *(--stack_top) = 0;                         // rbx
    *(--stack_top) = 0;                         // rbp
    *(--stack_top) = 0;                         // r12
    *(--stack_top) = 0;                         // r13
    *(--stack_top) = (uint64_t)arg;             // r14 (passed into thread_startup as arg)
    *(--stack_top) = (uint64_t)entry;           // r15 (passed into thread_startup as entry function)

    // Setup trampoline entry arguments via ABI registers or wrapper
    t->rsp = stack_top;

    queue_thread(t);
    return t;
}

void sched_init(void) {
    kernel_process = kzalloc(sizeof(process_t));
    kernel_process->pid = 0;
    strcpy(kernel_process->name, "kernel_idle");
    kernel_process->address_space = *vmm_get_kernel_address_space();

    // Create primary idle/kernel thread for current execution context
    current_thread = kzalloc(sizeof(thread_t));
    current_thread->tid = 0;
    strcpy(current_thread->name, "kernel_main");
    current_thread->state = THREAD_RUNNING;
    current_thread->time_slice = DEFAULT_TIME_SLICE;
    current_thread->remaining_ticks = DEFAULT_TIME_SLICE;
    current_thread->process = kernel_process;

    queue_thread(current_thread);
    current_thread->state = THREAD_RUNNING;

    scheduler_enabled = true;
    printk("SCHED: Preemptive scheduler initialized.\n");
}

void sched_yield(void) {
    if (!scheduler_enabled || !run_queue) return;

    cli();
    thread_t *prev = current_thread;
    thread_t *next = prev->next;

    // Find next runnable thread
    while (next && next->state != THREAD_READY && next->state != THREAD_RUNNING) {
        next = next->next;
        if (next == prev) break;
    }

    if (!next || next == prev || (next->state != THREAD_READY && next->state != THREAD_RUNNING)) {
        sti();
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }

    next->state = THREAD_RUNNING;
    current_thread = next;
    current_thread->remaining_ticks = current_thread->time_slice;

    // Switch TSS stack for interrupts/syscalls
    if (next->stack_base) {
        tss_set_rsp0((uint64_t)next->stack_base + next->stack_size);
    }

    // Switch address space if needed
    if (next->process != prev->process) {
        vmm_switch_address_space(&next->process->address_space);
    }

    hal_switch_context(&prev->rsp, next->rsp);
    sti();
}

void scheduler_tick(interrupt_frame_t *frame) {
    (void)frame;
    if (!scheduler_enabled || !current_thread) return;

    if (current_thread->remaining_ticks > 0) {
        current_thread->remaining_ticks--;
    }

    if (current_thread->remaining_ticks == 0) {
        sched_yield();
    }
}

thread_t *sched_get_current_thread(void) {
    return current_thread;
}

process_t *sched_get_current_process(void) {
    return current_thread ? current_thread->process : NULL;
}

void thread_exit(void) {
    cli();
    current_thread->state = THREAD_DEAD;
    dequeue_thread(current_thread);
    sched_yield();

    while (1) {
        hlt();
    }
}
