#include "fake_clock.h"

#include <stddef.h>
#include <stdint.h>

void fake_clock_init(fake_clock_t *clock, uint64_t initial_ms)
{
    if (clock != NULL) {
        clock->now_ms = initial_ms;
    }
}

void fake_clock_reset(fake_clock_t *clock)
{
    fake_clock_init(clock, 0U);
}

void fake_clock_set_ms(fake_clock_t *clock, uint64_t now_ms)
{
    fake_clock_init(clock, now_ms);
}

bool fake_clock_advance_ms(fake_clock_t *clock, uint64_t delta_ms)
{
    if (clock == NULL || delta_ms > UINT64_MAX - clock->now_ms) {
        return false;
    }

    clock->now_ms += delta_ms;
    return true;
}

uint64_t fake_clock_now_ms(void *context)
{
    const fake_clock_t *clock = context;
    return clock != NULL ? clock->now_ms : 0U;
}
