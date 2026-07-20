#ifndef BOTFARMS_FAKE_EVENT_SINK_H
#define BOTFARMS_FAKE_EVENT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAKE_EVENT_SINK_CAPACITY 64U

typedef struct {
    uint32_t event_id;
    int32_t subject;
    int64_t value;
    uint64_t timestamp_ms;
} fake_event_record_t;

typedef struct {
    fake_event_record_t records[FAKE_EVENT_SINK_CAPACITY];
    size_t count;
    size_t dropped_count;
} fake_event_sink_t;

void fake_event_sink_reset(fake_event_sink_t *sink);
bool fake_event_sink_capture(fake_event_sink_t *sink,
                             uint32_t event_id,
                             int32_t subject,
                             int64_t value,
                             uint64_t timestamp_ms);
size_t fake_event_sink_count(const fake_event_sink_t *sink);
size_t fake_event_sink_dropped_count(const fake_event_sink_t *sink);
const fake_event_record_t *fake_event_sink_get(const fake_event_sink_t *sink, size_t index);

#endif
