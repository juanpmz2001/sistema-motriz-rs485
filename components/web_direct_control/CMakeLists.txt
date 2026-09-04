idf_component_register(SRCS "web_direct_control.c" "web_direct_control_model.c"
                    INCLUDE_DIRS "include"
                    EMBED_FILES "web_ui.html"
                    REQUIRES esp_http_server json motion_application robot_capabilities wifi_manager)
