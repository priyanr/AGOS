#ifndef HAL_TIMER_H
#define HAL_TIMER_H

#include <kernel/types.h>

void timer_init(uint32_t frequency);
uint64_t timer_get_ticks(void);
void timer_sleep_ms(uint32_t ms);

#endif // HAL_TIMER_H
