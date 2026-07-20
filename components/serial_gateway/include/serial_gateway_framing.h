#ifndef SERIAL_GATEWAY_FRAMING_H
#define SERIAL_GATEWAY_FRAMING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_GATEWAY_COMMAND_MAX 160

typedef enum {
    SERIAL_GATEWAY_FRAME_NONE = 0,
    SERIAL_GATEWAY_FRAME_LINE_READY,
    SERIAL_GATEWAY_FRAME_LINE_TOO_LONG,
} serial_gateway_frame_event_t;

typedef struct {
    char line[SERIAL_GATEWAY_COMMAND_MAX];
    size_t length;
    bool discarding;
} serial_gateway_line_framer_t;

void serial_gateway_line_framer_init(serial_gateway_line_framer_t *framer);
serial_gateway_frame_event_t serial_gateway_line_framer_feed(serial_gateway_line_framer_t *framer,
                                                              char ch);
const char *serial_gateway_line_framer_line(const serial_gateway_line_framer_t *framer);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_GATEWAY_FRAMING_H
