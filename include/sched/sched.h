#ifndef SCHED_SCHED_H
#define SCHED_SCHED_H

#include <kernel/types.h>
#include <mm/vmm.h>


typedef enum {
    THREAD_EMBRYO,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_DEAD
} thread_state_t;

struct process;

typedef struct thread {
    uint64_t tid;
    char name[32];
    thread_state_t state;
    uint64_t *sp;          // Saved stack pointer while switched out (hal_switch_context)
    void *stack_base;
    size_t stack_size;
    uint32_t time_slice;
    uint32_t remaining_ticks;
    struct process *process;
    struct thread *next;
    struct thread *prev;
} thread_t;

typedef struct process {
    uint64_t pid;
    char name[32];
    address_space_t address_space;
    thread_t *threads;
    struct process *parent;
    struct process *next;
} process_t;

void sched_init(void);
thread_t *thread_create(process_t *proc, const char *name, void (*entry)(void *), void *arg);
process_t *process_create(const char *name);
void sched_yield(void);
void scheduler_tick(void); // Called by the architecture timer interrupt on every tick
thread_t *sched_get_current_thread(void);
process_t *sched_get_current_process(void);
void thread_exit(void);

#endif // SCHED_SCHED_H
