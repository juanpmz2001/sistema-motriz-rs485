#ifndef ACTUATION_COORDINATOR_H
#define ACTUATION_COORDINATOR_H
#include "robot_capabilities.h"
#define ACTUATION_COORDINATOR_MAX_SETPOINTS 16
typedef enum { ACTUATION_RESULT_SUCCESS=0, ACTUATION_RESULT_FAILURE, ACTUATION_RESULT_PARTIAL } actuation_result_t;
typedef struct { robot_endpoint_id_t endpoint_id; int16_t rpm; } actuation_velocity_request_t;
typedef struct { robot_endpoint_id_t endpoint_id; robot_capability_error_t error; bool applied; bool rollback_stop_attempted; robot_capability_error_t rollback_stop_error; } actuation_endpoint_result_t;
typedef struct { actuation_result_t outcome; size_t requested; size_t applied; actuation_endpoint_result_t endpoints[ACTUATION_COORDINATOR_MAX_SETPOINTS]; } actuation_report_t;
typedef struct { robot_endpoint_registry_t *registry; } actuation_coordinator_t;
void actuation_coordinator_init(actuation_coordinator_t*,robot_endpoint_registry_t*);
actuation_result_t actuation_coordinator_set_velocity_rpm(actuation_coordinator_t*,robot_endpoint_id_t,int16_t,actuation_report_t*);
actuation_result_t actuation_coordinator_apply_velocity_rpm(actuation_coordinator_t*,const actuation_velocity_request_t*,size_t,actuation_report_t*);
actuation_result_t actuation_coordinator_stop_endpoint(actuation_coordinator_t*,robot_endpoint_id_t,actuation_report_t*);
actuation_result_t actuation_coordinator_stop_all(actuation_coordinator_t*,actuation_report_t*);
#endif
