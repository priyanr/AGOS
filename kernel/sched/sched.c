#include <sched/sched.h>
#include <mm/heap.h>
#include <hal/platform.h>
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

    uint64_t *stack_top = (uint64_t *)((uintptr_t)t->stack_base + t->stack_size);
    t->sp = hal_thread_stack_init(stack_top, entry, arg);

    uint64_t flags = irq_save(); // the timer IRQ may walk the run queue
    queue_thread(t);
    irq_restore(flags);
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

    // Keep the caller's interrupt state: a yield from the timer IRQ must return with IRQs still masked
    uint64_t flags = irq_save();
    thread_t *prev = current_thread;
    // An exiting thread is already unlinked (next == NULL): continue from the queue head
    thread_t *next = prev->next ? prev->next : run_queue;

    // Find next runnable thread
    for (thread_t *start = next; next && next->state != THREAD_READY && next->state != THREAD_RUNNING;) {
        next = next->next;
        if (next == prev || next == start) break;
    }

    if (!next || next == prev || (next->state != THREAD_READY && next->state != THREAD_RUNNING)) {
        irq_restore(flags);
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }

    next->state = THREAD_RUNNING;
    current_thread = next;
    current_thread->remaining_ticks = current_thread->time_slice;

    // Stack for interrupts taken while the next thread runs (x86 TSS)
    if (next->stack_base) {
        hal_set_kernel_stack((uint64_t)next->stack_base + next->stack_size);
    }

    // Switch address space if needed
    if (next->process != prev->process) {
        vmm_switch_address_space(&next->process->address_space);
    }

    hal_switch_context(&prev->sp, next->sp);
    irq_restore(flags);
}

void scheduler_tick(void) {
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
