#ifndef BOTFARMS_HOST_TEST_H
#define BOTFARMS_HOST_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef bool (*host_test_function_t)(void);

typedef struct {
    const char *name;
    host_test_function_t function;
} host_test_case_t;

typedef struct {
    size_t total;
    size_t passed;
    size_t failed;
} host_test_summary_t;

host_test_summary_t host_test_run_cases(const host_test_case_t *cases,
                                        size_t case_count,
                                        FILE *output);
int host_test_exit_code(host_test_summary_t summary);

#define HOST_TEST_ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define HOST_TEST_CASE(function_name) { #function_name, function_name }

#define HOST_TEST_CHECK(condition)                                                          \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr,                                                                 \
                    "ASSERT %s:%d: %s\n",                                                \
                    __FILE__,                                                               \
                    __LINE__,                                                               \
                    #condition);                                                            \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

#endif
