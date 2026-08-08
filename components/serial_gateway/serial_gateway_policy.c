#include "serial_gateway_policy.h"

#include <stddef.h>
#include <strings.h>

static bool arg_is(const char *value, const char *expected)
{
    return value && expected && strcasecmp(value, expected) == 0;
}

static bool no_arg_command_allowed(const char *command)
{
    static const char *const allowed[] = {
        "PING",
        "VERSION",
        "PROFILE_STATUS",
        "COMPOSITION_STATUS",
        "PLATFORM_STATUS",
        "SAFETY_STATUS",
        "CONFIG_STATUS",
        "WIFI_STATUS",
        "OTA_CONFIG",
        "OTA_ANNOUNCE_STATUS",
        "OTA_ROLLBACK_STATUS",
        "OTA_AUTO_STATUS",
        "IBUS_STATUS",
        "IBUS_CHANNELS",
        "POLL_ONCE",
        "MAINT_LAN_STATUS",
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (arg_is(command, allowed[i])) {
            return true;
        }
    }
    return false;
}

bool serial_gateway_lan_command_allowed(int argc, const char *const argv[])
{
    if (argc <= 0 || !argv || !argv[0]) {
        return false;
    }
    if (argc == 1 && no_arg_command_allowed(argv[0])) {
        return true;
    }
    if ((arg_is(argv[0], "GET_SPEED") || arg_is(argv[0], "GET_MOTOR")) && argc == 2) {
        return true;
    }
    if (arg_is(argv[0], "STOP") && argc == 2) {
        return true;
    }
    if (arg_is(argv[0], "SET_SPEED") && argc == 3) {
        return true;
    }
    if (arg_is(argv[0], "READ_REG") && (argc == 3 || argc == 4)) {
        return true;
    }
    if (arg_is(argv[0], "GET_SVD48_CONFIG") && (argc == 2 || argc == 3)) {
        return true;
    }
    if (arg_is(argv[0], "WRITE_REG") && argc == 5 && arg_is(argv[4], "CONFIRM")) {
        return true;
    }
    if (arg_is(argv[0], "WRITE_REGS") && argc >= 5 && arg_is(argv[argc - 1], "CONFIRM")) {
        return true;
    }
    if (arg_is(argv[0], "SAVE_SVD48_CONFIG") && argc == 3 && arg_is(argv[2], "CONFIRM")) {
        return true;
    }
    if (arg_is(argv[0], "SET_SVD48_GEAR_RATIO") && argc == 5 && arg_is(argv[4], "CONFIRM")) {
        return true;
    }
    if (arg_is(argv[0], "SVD48_IDENTIFY_STATUS") && argc == 3) {
        return true;
    }
    if (arg_is(argv[0], "SVD48_IDENTIFY") && argc == 5 && arg_is(argv[4], "CONFIRM")) {
        return true;
    }
    return false;
}

bool serial_gateway_diagnostic_command_allowed(int argc,
                                               const char *const argv[])
{
    static const char *const read_only[] = {
        "PING",
        "VERSION",
        "HELP",
        "PLATFORM_STATUS",
        "CONFIG_STATUS",
        "WIFI_STATUS",
        "PROFILE_STATUS",
        "COMPOSITION_STATUS",
    };
    if (argc <= 0 || !argv || !argv[0]) {
        return false;
    }
    if (argc == 1) {
        for (size_t index = 0;
             index < sizeof(read_only) / sizeof(read_only[0]);
             ++index) {
            if (arg_is(argv[0], read_only[index])) {
                return true;
            }
        }
    }
    return argc == 2 && arg_is(argv[0], "STOP") && arg_is(argv[1], "ALL");
}
