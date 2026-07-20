#ifndef BOTFARMS_FAKE_CLOCK_H
#define BOTFARMS_FAKE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t now_ms;
} fake_clock_t;

void fake_clock_init(fake_clock_t *clock, uint64_t initial_ms);
void fake_clock_reset(fake_clock_t *clock);
void fake_clock_set_ms(fake_clock_t *clock, uint64_t now_ms);
bool fake_clock_advance_ms(fake_clock_t *clock, uint64_t delta_ms);
uint64_t fake_clock_now_ms(void *context);

#endif
