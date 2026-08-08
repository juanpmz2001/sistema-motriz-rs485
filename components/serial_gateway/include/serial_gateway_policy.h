#ifndef SERIAL_GATEWAY_POLICY_H
#define SERIAL_GATEWAY_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool serial_gateway_lan_command_allowed(int argc, const char *const argv[]);
bool serial_gateway_diagnostic_command_allowed(int argc,
                                               const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif // SERIAL_GATEWAY_POLICY_H
