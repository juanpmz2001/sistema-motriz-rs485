# SVD48 Read-Only Inventory

- Captured: `2026-07-21T03:56:12.678785+00:00`
- ESP: `192.168.1.185:32321`
- Drive ID: `2`
- Requests: `187`
- Successful: `151`
- Failed/unsupported: `36`
- Operation: read-only `READ_REG`; no writes or movement commands.

For two-word values, both word orders are preserved in the JSON because
the controller's float order has not been physically verified.

| Group | Name | Address | Type | Result | Values |
| --- | --- | --- | --- | --- | --- |
| throttle | throttle_dead_zone | 0x2000 x1 | u16 | OK | 0x0002 |
| throttle | throttle_curve_mode | 0x2001 x1 | u16 | OK | 0x0000 |
| throttle | throttle_curve_points | 0x2002 x1 | u16 | OK | 0x0002 |
| throttle | normal_forward_force | 0x2010 x1 | u16 | OK | 0x0064 |
| throttle | normal_reverse_force | 0x2011 x1 | u16 | OK | 0x000A |
| throttle | normal_acceleration | 0x2012 x1 | u16 | OK | 0x0032 |
| throttle | normal_max_speed | 0x2013 x1 | u16 | OK | 0x0005 |
| throttle | sport_forward_force | 0x2020 x1 | u16 | OK | 0x0064 |
| throttle | sport_reverse_force | 0x2021 x1 | u16 | OK | 0x000A |
| throttle | sport_acceleration | 0x2022 x1 | u16 | OK | 0x0032 |
| throttle | sport_max_speed | 0x2023 x1 | u16 | OK | 0x0005 |
| throttle | turbo_forward_force | 0x2030 x1 | u16 | OK | 0x0064 |
| throttle | turbo_reverse_force | 0x2031 x1 | u16 | OK | 0x000A |
| throttle | turbo_acceleration | 0x2032 x1 | u16 | OK | 0x0032 |
| throttle | turbo_max_speed | 0x2033 x1 | u16 | OK | 0x0005 |
| throttle | throttle_curve_00_15 | 0x2060 x16 | u16[16] | OK | 0x0000 0x0064 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 |
| throttle | throttle_curve_16_31 | 0x2070 x16 | u16[16] | OK | 0x0000 0x0064 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 |
| brake | brake_dead_zone | 0x2080 x1 | u16 | OK | 0x0002 |
| brake | brake_curve_mode | 0x2081 x1 | u16 | OK | 0x0000 |
| brake | brake_curve_points | 0x2082 x1 | u16 | OK | 0x0002 |
| brake | normal_brake_force | 0x2090 x1 | u16 | OK | 0x0064 |
| brake | normal_brake_ramp | 0x2091 x1 | u16 | OK | 0x000A |
| brake | normal_brake_speed | 0x2092 x1 | u16 | OK | 0x0032 |
| brake | sport_brake_force | 0x20A0 x1 | u16 | OK | 0x0064 |
| brake | sport_brake_ramp | 0x20A1 x1 | u16 | OK | 0x000A |
| brake | sport_brake_speed | 0x20A2 x1 | u16 | OK | 0x0032 |
| brake | turbo_brake_force | 0x20B0 x1 | u16 | OK | 0x0064 |
| brake | turbo_brake_ramp | 0x20B1 x1 | u16 | OK | 0x000A |
| brake | turbo_brake_speed | 0x20B2 x1 | u16 | OK | 0x0032 |
| brake | brake_curve_00_15 | 0x20E0 x16 | u16[16] | OK | 0x0000 0x0064 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 |
| brake | brake_curve_16_31 | 0x20F0 x16 | u16[16] | OK | 0x0000 0x0064 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 |
| remote_vehicle | remote_mode | 0x2100 x1 | u16 | OK | 0x0000 |
| remote_vehicle | remote_direction | 0x2101 x1 | u16 | OK | 0x0000 |
| remote_vehicle | throttle_stroke | 0x2102 x1 | u16 | OK | 0x3A98 |
| remote_vehicle | brake_stroke | 0x2103 x1 | u16 | OK | 0x3A98 |
| remote_vehicle | vehicle_speed | 0x2130 x1 | u16 | OK | 0x0000 |
| remote_vehicle | battery_level | 0x2131 x1 | u16 | OK | 0x0218 |
| remote_vehicle | vehicle_power | 0x2132 x1 | u16 | OK | 0x0000 |
| remote_vehicle | m1_temperature | 0x2133 x1 | u16 | OK | 0xFF1D |
| remote_vehicle | m2_temperature | 0x2134 x1 | u16 | OK | 0xFF1D |
| remote_vehicle | drive_temperature | 0x2135 x1 | u16 | OK | 0x00E0 |
| vehicle | maximum_acceleration | 0x2200 x1 | u16 | OK | 0x0190 |
| vehicle | wheel_diameter_mm | 0x2201 x1 | u16 | OK | 0x0064 |
| vehicle | motor_teeth | 0x2202 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2202 COUNT:1 ERR:0x108 |
| vehicle | wheel_teeth | 0x2203 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2203 COUNT:1 ERR:0x108 |
| vehicle | throttle_input_type | 0x2280 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2280 COUNT:1 ERR:0x108 |
| vehicle | ppm_minimum | 0x2281 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2281 COUNT:1 ERR:0x108 |
| vehicle | ppm_center | 0x2282 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2282 COUNT:1 ERR:0x108 |
| vehicle | ppm_maximum | 0x2283 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x2283 COUNT:1 ERR:0x108 |
| board | slave_id | 0x3001 x1 | u16 | OK | 0x0002 |
| board | software_version | 0x3002 x1 | u16 | OK | 0x0131 |
| board | hardware_version | 0x3003 x1 | u16 | OK | 0x0300 |
| board | bootloader_version | 0x3004 x1 | u16 | OK | 0x0103 |
| board | product_id | 0x3005 x1 | u16 | OK | 0x0101 |
| board | rs485_baud_enum | 0x3006 x1 | u16 | OK | 0x0004 |
| board | can_baud_enum | 0x3007 x1 | u16 | OK | 0x0006 |
| board | control_input_source | 0x3008 x1 | u16 | OK | 0x0001 |
| board | maximum_bus_voltage_dv | 0x3009 x1 | u16 | OK | 0x0258 |
| board | overload_timeout_ms | 0x300A x1 | u16 | OK | 0x03E8 |
| board | power_on_encoder_calibration | 0x300B x1 | u16 | OK | 0x0001 |
| board | save_parameters_command | 0x3100 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3100 COUNT:1 ERR:0x108 |
| board | heartbeat_or_in_position | 0x3180 x1 | u16 | OK | 0x0001 |
| can_upload | can_active_packet_0 | 0x3200 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3200 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_1 | 0x3202 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3202 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_2 | 0x3204 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3204 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_3 | 0x3206 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3206 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_4 | 0x3208 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3208 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_5 | 0x320A x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x320a COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_6 | 0x320C x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x320c COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_7 | 0x320E x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x320e COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_8 | 0x3210 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3210 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_9 | 0x3212 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3212 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_10 | 0x3214 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3214 COUNT:2 ERR:0x108 |
| can_upload | can_active_packet_11 | 0x3216 x2 | u32 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3216 COUNT:2 ERR:0x108 |
| rs232_upload | rs232_upload_0x3300 | 0x3300 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3300 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3301 | 0x3301 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3301 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3302 | 0x3302 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3302 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3303 | 0x3303 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3303 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3304 | 0x3304 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3304 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3305 | 0x3305 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3305 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3306 | 0x3306 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3306 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3307 | 0x3307 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3307 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3308 | 0x3308 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3308 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x3309 | 0x3309 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x3309 COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x330a | 0x330A x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x330a COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x330b | 0x330B x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x330b COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x330c | 0x330C x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x330c COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x330d | 0x330D x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x330d COUNT:1 ERR:0x108 |
| rs232_upload | rs232_upload_0x330e | 0x330E x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x330e COUNT:1 ERR:0x108 |
| motor_electrical | m1_lq | 0x5000 x2 | float32_words | OK | 0x399B 0xD183 |
| motor_electrical | m2_lq | 0x5002 x2 | float32_words | OK | 0x3995 0x9451 |
| motor_electrical | m1_ld | 0x5008 x2 | float32_words | OK | 0x399B 0xD183 |
| motor_electrical | m2_ld | 0x500A x2 | float32_words | OK | 0x3995 0x9451 |
| motor_electrical | m1_rs | 0x5010 x2 | float32_words | OK | 0x3DF2 0x4746 |
| motor_electrical | m2_rs | 0x5012 x2 | float32_words | OK | 0x3DD7 0xDBF5 |
| motor_general | m1_pole_pairs | 0x5018 x1 | u16 | OK | 0x0018 |
| motor_general | m2_pole_pairs | 0x5019 x1 | u16 | OK | 0x0018 |
| motor_general | m1_max_speed_rpm | 0x501C x1 | u16 | OK | 0x0064 |
| motor_general | m2_max_speed_rpm | 0x501D x1 | u16 | OK | 0x0064 |
| motor_general | m1_max_current_a | 0x5020 x1 | u16 | OK | 0x001E |
| motor_general | m2_max_current_a | 0x5021 x1 | u16 | OK | 0x001E |
| motor_general | m1_kv_tenth_rpm_per_v | 0x5024 x1 | u16 | OK | 0x00A6 |
| motor_general | m2_kv_tenth_rpm_per_v | 0x5025 x1 | u16 | OK | 0x00A6 |
| motor_general | m1_rotation_direction | 0x5028 x1 | u16 | OK | 0x0001 |
| motor_general | m2_rotation_direction | 0x5029 x1 | u16 | OK | 0x0001 |
| motor_general | m1_sensor_type | 0x502C x1 | u16 | OK | 0x0001 |
| motor_general | m2_sensor_type | 0x502D x1 | u16 | OK | 0x0001 |
| motion_config | m1_control_mode | 0x5100 x1 | u16 | OK | 0x0000 |
| motion_config | m2_control_mode | 0x5101 x1 | u16 | OK | 0x0000 |
| motion_config | m1_position_mode | 0x5104 x1 | u16 | OK | 0x0001 |
| motion_config | m2_position_mode | 0x5105 x1 | u16 | OK | 0x0001 |
| motion_config | m1_max_acceleration_rpm_s | 0x5108 x1 | u16 | OK | 0x002D |
| motion_config | m2_max_acceleration_rpm_s | 0x5109 x1 | u16 | OK | 0x002D |
| motion_config | m1_max_deceleration_rpm_s | 0x510C x1 | u16 | OK | 0x0028 |
| motion_config | m2_max_deceleration_rpm_s | 0x510D x1 | u16 | OK | 0x0028 |
| motion_config | m1_s_curve_ms | 0x5110 x1 | u16 | OK | 0x0064 |
| motion_config | m2_s_curve_ms | 0x5111 x1 | u16 | OK | 0x0064 |
| pid | m1_speed_kp | 0x5200 x2 | float32_words | OK | 0x3E99 0x999A |
| pid | m2_speed_kp | 0x5202 x2 | float32_words | OK | 0x3E99 0x999A |
| pid | m1_speed_ki | 0x5208 x2 | float32_words | OK | 0x3DCC 0xCCCD |
| pid | m2_speed_ki | 0x520A x2 | float32_words | OK | 0x3DCC 0xCCCD |
| pid | m1_speed_kd | 0x5210 x2 | float32_words | OK | 0x4000 0x0000 |
| pid | m2_speed_kd | 0x5212 x2 | float32_words | OK | 0x4000 0x0000 |
| pid | m1_position_kp | 0x5218 x2 | float32_words | OK | 0x41C8 0x0000 |
| pid | m2_position_kp | 0x521A x2 | float32_words | OK | 0x41C8 0x0000 |
| pid | m1_position_ki | 0x5220 x2 | float32_words | OK | 0x0000 0x0000 |
| pid | m2_position_ki | 0x5222 x2 | float32_words | OK | 0x0000 0x0000 |
| pid | m1_position_kd | 0x5228 x2 | float32_words | OK | 0x4120 0x0000 |
| pid | m2_position_kd | 0x522A x2 | float32_words | OK | 0x4120 0x0000 |
| pid | m1_current_loop_gain | 0x5230 x2 | float32_words | OK | 0x3E80 0x0000 |
| pid | m2_current_loop_gain | 0x5232 x2 | float32_words | OK | 0x3E80 0x0000 |
| pid | m1_speed_feed_forward | 0x5238 x2 | float32_words | OK | 0x3F80 0x0000 |
| pid | m2_speed_feed_forward | 0x523A x2 | float32_words | OK | 0x3F80 0x0000 |
| pid | m1_speed_dead_zone | 0x5240 x1 | u16 | OK | 0x0000 |
| pid | m2_speed_dead_zone | 0x5241 x1 | u16 | OK | 0x0000 |
| command_state | m1_control_command | 0x5300 x1 | u16 | OK | 0x0000 |
| command_state | m2_control_command | 0x5301 x1 | u16 | OK | 0x0000 |
| command_state | m1_given_speed_rpm | 0x5304 x1 | u16 | OK | 0x0000 |
| command_state | m2_given_speed_rpm | 0x5305 x1 | u16 | OK | 0x0000 |
| command_state | m1_given_current_da | 0x5308 x1 | u16 | OK | 0x0000 |
| command_state | m2_given_current_da | 0x5309 x1 | u16 | OK | 0x0000 |
| command_state | m1_given_position | 0x530C x2 | i32 | OK | 0x0000 0x0000 |
| command_state | m2_given_position | 0x530E x2 | i32 | OK | 0x0000 0x0000 |
| telemetry | m1_status | 0x5400 x1 | u16 | OK | 0x0000 |
| telemetry | m2_status | 0x5401 x1 | u16 | OK | 0x0000 |
| telemetry | m1_motor_temperature_dc | 0x5404 x1 | u16 | OK | 0xFF1D |
| telemetry | m2_motor_temperature_dc | 0x5405 x1 | u16 | OK | 0xFF1D |
| telemetry | m1_mos_temperature_dc | 0x5408 x1 | i16 | OK | 0x00E0 |
| telemetry | m2_mos_temperature_dc | 0x5409 x1 | i16 | OK | 0x00E0 |
| telemetry | m1_bus_voltage_dv | 0x540C x1 | u16 | OK | 0x0218 |
| telemetry | m2_bus_voltage_dv | 0x540D x1 | u16 | OK | 0x0218 |
| telemetry | m1_actual_speed_rpm | 0x5410 x1 | u16 | OK | 0x0000 |
| telemetry | m2_actual_speed_rpm | 0x5411 x1 | u16 | OK | 0x0000 |
| telemetry | m1_actual_current_da | 0x5414 x1 | u16 | OK | 0x0000 |
| telemetry | m2_actual_current_da | 0x5415 x1 | u16 | OK | 0x0000 |
| telemetry | m1_position | 0x5418 x2 | i32 | OK | 0xFFFF 0xFEF8 |
| telemetry | m2_position | 0x541A x2 | i32 | OK | 0x0000 0x0015 |
| telemetry | m1_error | 0x5420 x2 | u32 | OK | 0x0000 0x0000 |
| telemetry | m2_error | 0x5422 x2 | u32 | OK | 0x0000 0x0000 |
| encoder | m1_calibration_command | 0x5500 x1 | u16 | OK | 0x0000 |
| encoder | m2_calibration_command | 0x5501 x1 | u16 | OK | 0x0000 |
| encoder | encoder_lines_or_bits | 0x5504 x1 | u16 | OK | 0x0400 |
| encoder | m1_installation_direction | 0x5508 x1 | u16 | OK | 0x0000 |
| encoder | m2_installation_direction | 0x5509 x1 | u16 | OK | 0x0000 |
| encoder | m1_encoder_bias_deg | 0x550C x1 | u16 | OK | 0x002B |
| encoder | m2_encoder_bias_deg | 0x550D x1 | u16 | OK | 0x002C |
| encoder | m1_encoder_temperature_dc | 0x5580 x1 | u16 | OK | 0x0000 |
| encoder | m2_encoder_temperature_dc | 0x5581 x1 | u16 | OK | 0x0000 |
| encoder | m1_encoder_calibration_status | 0x5584 x1 | u16 | OK | 0x0000 |
| encoder | m2_encoder_calibration_status | 0x5585 x1 | u16 | OK | 0x0000 |
| hall | m1_calibration_command | 0x5600 x1 | u16 | OK | 0x0000 |
| hall | m2_calibration_command | 0x5601 x1 | u16 | OK | 0x0000 |
| hall | m2_calibration_current_candidate_a | 0x5605 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x5605 COUNT:1 ERR:0x108 |
| hall | m2_calibration_current_candidate_b | 0x5609 x1 | u16 | ERR | ERR READ_REG_FAILED DRIVE:2 REG:0x5609 COUNT:1 ERR:0x108 |
| hall | m1_installation_120_or_60 | 0x5620 x1 | u16 | OK | 0x0000 |
| hall | m2_installation_120_or_60 | 0x5621 x1 | u16 | OK | 0x0000 |
| hall | m1_calibration_current | 0x5624 x1 | u16 | OK | 0x000F |
| hall | m1_angle_table | 0x5640 x8 | i16[8] | OK | 0x0000 0x002C 0x0067 0x00A4 0x00E0 0x011B 0x0159 0x0000 |
| hall | m2_angle_table | 0x5650 x8 | i16[8] | OK | 0x0000 0x002C 0x0067 0x00A4 0x00E0 0x011B 0x015A 0x0000 |
| hall | m1_sensor_temperature_dc | 0x5680 x1 | u16 | OK | 0x0000 |
| hall | m2_sensor_temperature_dc | 0x5681 x1 | u16 | OK | 0x0000 |
| hall | m1_calibration_status | 0x5684 x1 | u16 | OK | 0x0000 |
| hall | m2_calibration_status | 0x5685 x1 | u16 | OK | 0x0000 |
| hall | m1_hall_status | 0x5688 x1 | u16 | OK | 0x011B |
| hall | m2_hall_status | 0x5689 x1 | u16 | OK | 0x00A4 |
| hall | m1_current_angle_deg | 0x568C x1 | u16 | OK | 0x0159 |
| hall | m2_current_angle_deg | 0x568D x1 | u16 | OK | 0x0067 |
