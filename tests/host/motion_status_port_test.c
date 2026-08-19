#include "host_test.h"
#include "motion_status_port.h"

typedef struct {
    motion_status_port_t port;
    motion_status_snapshot_t snapshot;
    bool succeeds;
} fake_motion_status_t;

static bool fake_snapshot(motion_status_port_t *port,
                          motion_status_snapshot_t *snapshot)
{
    fake_motion_status_t *fake = (fake_motion_status_t *)port->context;
    if (!fake->succeeds) {
        return false;
    }
    *snapshot = fake->snapshot;
    return true;
}

static void fake_status_init(fake_motion_status_t *fake,
                             motion_control_state_t state,
                             bool available,
                             bool succeeds)
{
    static const motion_status_ops_t ops = { .snapshot = fake_snapshot };
    *fake = (fake_motion_status_t){
        .snapshot = { .available = available, .state = state },
        .succeeds = succeeds,
    };
    fake->port.ops = &ops;
    fake->port.context = fake;
}

static bool test_armed_and_active_block_maintenance_changes(void)
{
    motion_control_state_t observed = MOTION_CONTROL_UNAVAILABLE;
    fake_motion_status_t armed;
    fake_status_init(&armed, MOTION_CONTROL_ARMED, true, true);
    HOST_TEST_CHECK(motion_status_blocks_maintenance_changes(&armed.port,
                                                              &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_ARMED);

    fake_motion_status_t active;
    fake_status_init(&active, MOTION_CONTROL_ACTIVE, true, true);
    HOST_TEST_CHECK(motion_status_blocks_maintenance_changes(&active.port,
                                                              &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_ACTIVE);
    return true;
}

static bool test_disarmed_and_unavailable_allow_maintenance_changes(void)
{
    motion_control_state_t observed = MOTION_CONTROL_ACTIVE;
    fake_motion_status_t disarmed;
    fake_status_init(&disarmed, MOTION_CONTROL_DISARMED, true, true);
    HOST_TEST_CHECK(!motion_status_blocks_maintenance_changes(&disarmed.port,
                                                               &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_DISARMED);

    fake_motion_status_t unavailable;
    fake_status_init(&unavailable, MOTION_CONTROL_ACTIVE, false, true);
    HOST_TEST_CHECK(!motion_status_blocks_maintenance_changes(&unavailable.port,
                                                               &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_UNAVAILABLE);
    HOST_TEST_CHECK(!motion_status_blocks_maintenance_changes(NULL, &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_UNAVAILABLE);

    fake_motion_status_t failed;
    fake_status_init(&failed, MOTION_CONTROL_ACTIVE, true, false);
    HOST_TEST_CHECK(!motion_status_blocks_maintenance_changes(&failed.port,
                                                               &observed));
    HOST_TEST_CHECK(observed == MOTION_CONTROL_UNAVAILABLE);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_armed_and_active_block_maintenance_changes),
        HOST_TEST_CASE(test_disarmed_and_unavailable_allow_maintenance_changes),
    };
    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
