#include "robot_composition.h"
#include "robot_control_endpoint_adapter.h"
#include <stdlib.h>
typedef struct robot_control_endpoint_adapter_storage { robot_control_endpoint_adapter_t adapters[ROBOT_PROFILE_MAX_ENDPOINTS]; } storage_t;
static int set_speed(void*c,uint8_t m,int16_t rpm){return (int)robot_control_set_motor_speed((robot_control_handle_t)c,m,rpm);}
static int stop_motor(void*c,uint8_t m){return (int)robot_control_stop_motor((robot_control_handle_t)c,m);}
robot_endpoint_id_t robot_composition_motor_endpoint_id(const robot_profile_t*p,uint8_t motor){if(!p)return 0;for(size_t i=0;i<p->endpoint_count;i++)if(p->endpoints[i].legacy_motor_index==motor)return p->endpoints[i].id;return 0;}
esp_err_t robot_composition_init(robot_composition_t*c,const robot_profile_t*p,robot_control_handle_t legacy){if(!c||!p||!legacy||robot_profile_validate(p)!=ROBOT_PROFILE_VALID)return ESP_ERR_INVALID_ARG;robot_endpoint_registry_init(&c->registry);c->storage=calloc(1,sizeof(storage_t));if(!c->storage)return ESP_ERR_NO_MEM;storage_t*s=(storage_t*)c->storage;for(size_t i=0;i<p->endpoint_count;i++){const robot_endpoint_profile_t*e=&p->endpoints[i];if(!robot_control_endpoint_adapter_init(&s->adapters[i],legacy,e->legacy_motor_index,e->id,e->name,e->criticality,e->min_rpm,e->max_rpm,set_speed,stop_motor)||robot_endpoint_registry_add(&c->registry,&s->adapters[i].endpoint)!=ROBOT_REGISTRY_OK){robot_composition_deinit(c);return ESP_ERR_INVALID_ARG;}}actuation_coordinator_init(&c->coordinator,&c->registry);return ESP_OK;}
void robot_composition_deinit(robot_composition_t*c){if(c){free(c->storage);c->storage=0;robot_endpoint_registry_init(&c->registry);c->coordinator.registry=0;}}
