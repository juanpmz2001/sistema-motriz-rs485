#include "host_test.h"

#include <stdlib.h>

host_test_summary_t host_test_run_cases(const host_test_case_t *cases,
                                        size_t case_count,
                                        FILE *output)
{
    host_test_summary_t summary = {
        .total = case_count,
        .passed = 0U,
        .failed = 0U,
    };

    if (cases == NULL && case_count > 0U) {
        summary.failed = case_count;
        if (output != NULL) {
            fprintf(output, "[FAIL] invalid test case array\n");
        }
        return summary;
    }

    for (size_t index = 0U; index < case_count; index++) {
        const char *name = cases[index].name != NULL ? cases[index].name : "unnamed";
        bool passed = cases[index].function != NULL && cases[index].function();

        if (passed) {
            summary.passed++;
        } else {
            summary.failed++;
        }

        if (output != NULL) {
            fprintf(output, "[%s] %s\n", passed ? "PASS" : "FAIL", name);
        }
    }

    if (output != NULL) {
        fprintf(output,
                "summary: %zu passed, %zu failed, %zu total\n",
                summary.passed,
                summary.failed,
                summary.total);
    }

    return summary;
}

int host_test_exit_code(host_test_summary_t summary)
{
    return summary.failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
