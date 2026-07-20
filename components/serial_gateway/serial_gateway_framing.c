#include "serial_gateway_framing.h"

#include <string.h>

void serial_gateway_line_framer_init(serial_gateway_line_framer_t *framer)
{
    if (framer) {
        memset(framer, 0, sizeof(*framer));
    }
}

serial_gateway_frame_event_t serial_gateway_line_framer_feed(serial_gateway_line_framer_t *framer,
                                                              char ch)
{
    if (!framer) {
        return SERIAL_GATEWAY_FRAME_NONE;
    }

    if (ch == '\r' || ch == '\n') {
        if (framer->discarding) {
            framer->discarding = false;
            framer->length = 0;
            framer->line[0] = '\0';
            return SERIAL_GATEWAY_FRAME_LINE_TOO_LONG;
        }
        if (framer->length == 0) {
            return SERIAL_GATEWAY_FRAME_NONE;
        }

        framer->line[framer->length] = '\0';
        framer->length = 0;
        return SERIAL_GATEWAY_FRAME_LINE_READY;
    }

    if (framer->discarding) {
        return SERIAL_GATEWAY_FRAME_NONE;
    }
    if (framer->length >= sizeof(framer->line) - 1) {
        framer->discarding = true;
        framer->length = 0;
        framer->line[0] = '\0';
        return SERIAL_GATEWAY_FRAME_NONE;
    }

    framer->line[framer->length++] = ch;
    return SERIAL_GATEWAY_FRAME_NONE;
}

const char *serial_gateway_line_framer_line(const serial_gateway_line_framer_t *framer)
{
    return framer ? framer->line : NULL;
}
