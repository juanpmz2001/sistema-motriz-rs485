#include "fake_event_sink.h"

#include <string.h>

void fake_event_sink_reset(fake_event_sink_t *sink)
{
    if (sink != NULL) {
        memset(sink, 0, sizeof(*sink));
    }
}

bool fake_event_sink_capture(fake_event_sink_t *sink,
                             uint32_t event_id,
                             int32_t subject,
                             int64_t value,
                             uint64_t timestamp_ms)
{
    if (sink == NULL) {
        return false;
    }

    if (sink->count >= FAKE_EVENT_SINK_CAPACITY) {
        sink->dropped_count++;
        return false;
    }

    sink->records[sink->count] = (fake_event_record_t) {
        .event_id = event_id,
        .subject = subject,
        .value = value,
        .timestamp_ms = timestamp_ms,
    };
    sink->count++;
    return true;
}

size_t fake_event_sink_count(const fake_event_sink_t *sink)
{
    return sink != NULL ? sink->count : 0U;
}

size_t fake_event_sink_dropped_count(const fake_event_sink_t *sink)
{
    return sink != NULL ? sink->dropped_count : 0U;
}

const fake_event_record_t *fake_event_sink_get(const fake_event_sink_t *sink, size_t index)
{
    if (sink == NULL || index >= sink->count) {
        return NULL;
    }

    return &sink->records[index];
}
