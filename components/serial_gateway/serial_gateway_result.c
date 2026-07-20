#include "serial_gateway_result.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool serial_gateway_error_code_from_line(const char *line, char *out_code, size_t out_code_size)
{
    if (!line || !out_code || out_code_size == 0) {
        return false;
    }

    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (strncmp(line, "ERR", 3) != 0 || (line[3] != '\0' && !isspace((unsigned char)line[3]))) {
        return false;
    }

    line += 3;
    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (*line == '\0') {
        snprintf(out_code, out_code_size, "COMMAND_ERROR");
        return true;
    }

    size_t length = 0;
    while (line[length] && !isspace((unsigned char)line[length])) {
        length++;
    }
    if (length >= out_code_size) {
        length = out_code_size - 1;
    }
    memcpy(out_code, line, length);
    out_code[length] = '\0';
    return true;
}
