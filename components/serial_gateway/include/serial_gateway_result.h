#ifndef SERIAL_GATEWAY_RESULT_H
#define SERIAL_GATEWAY_RESULT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool serial_gateway_error_code_from_line(const char *line, char *out_code, size_t out_code_size);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_GATEWAY_RESULT_H
