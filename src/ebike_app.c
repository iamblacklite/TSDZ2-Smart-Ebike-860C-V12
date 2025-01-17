/*
 * TongSheng TSDZ2 motor controller firmware
 *
 * Copyright (C) Casainho, Leon, MSpider65 2020.
 *
 * Released under the GPL License, Version 3
 */

#include "ebike_app.h"
#include <stdint.h>
#include <stdio.h>
#include "stm8s.h"
#include "stm8s_gpio.h"
#include "main.h"
#include "interrupts.h"
#include "adc.h"
#include "main.h"
#include "motor.h"
#include "pwm.h"
#include "uart.h"
#include "brake.h"
#include "lights.h"
#include "common.h"

// from v.1.1.0
// Error state (changed)
#define NO_ERROR                                0			// "None"
#define ERROR_NOT_INIT                          1			// "Motor not init"
#define ERROR_TORQUE_SENSOR                     (1 << 1)	// "Torque Fault"
#define ERROR_CADENCE_SENSOR		    		(1 << 2)	// "Cadence fault"
#define ERROR_MOTOR_BLOCKED     				(1 << 3)	// "Motor Blocked"
#define ERROR_BATTERY_OVERCURRENT               (1 << 5)    // "Battery Overcurrent"
#define ERROR_FATAL                             (1 << 6)	// "Comms"
#define ERROR_SPEED_SENSOR	                    (1 << 7)	// "Speed fault"

// Motor init state
#define MOTOR_INIT_STATE_RESET                  0
#define MOTOR_INIT_STATE_NO_INIT                1
#define MOTOR_INIT_STATE_INIT_START_DELAY       2
#define MOTOR_INIT_STATE_INIT_WAIT_DELAY        3
#define MOTOR_INIT_OK                           4

// Motor init status
#define MOTOR_INIT_STATUS_RESET                 0
#define MOTOR_INIT_STATUS_GOT_CONFIG            1
#define MOTOR_INIT_STATUS_INIT_OK               2

// Communications package frame type
#define COMM_FRAME_TYPE_ALIVE                         0
#define COMM_FRAME_TYPE_STATUS                        1
#define COMM_FRAME_TYPE_PERIODIC                      2
#define COMM_FRAME_TYPE_CONFIGURATIONS                3
#define COMM_FRAME_TYPE_FIRMWARE_VERSION              4
#define COMM_FRAME_TYPE_HALL_CALBRATION               5

// variables for various system functions
volatile uint8_t ui8_m_system_state = ERROR_NOT_INIT; // start with system error because configurations are empty at startup
volatile uint8_t ui8_m_motor_init_state = MOTOR_INIT_STATE_RESET;
volatile uint8_t ui8_m_motor_init_status = MOTOR_INIT_STATUS_RESET;
static uint8_t m_ui8_got_configurations_timer = 0;
static uint8_t ui8_configurations_changed = 0;

// Initial configuration values
volatile struct_configuration_variables m_configuration_variables = {
        .ui16_battery_low_voltage_cut_off_x10 = 300, // 36 V battery, 30.0V (3.0 * 10)
        .ui16_wheel_perimeter = 2050,                // 26'' wheel: 2050 mm perimeter
        .ui8_wheel_speed_max = 25,                   // 25 Km/h
        .ui8_foc_angle_multiplicator = 27,           // 36V motor 76 uH
        .ui8_pedal_torque_per_10_bit_ADC_step_x100 = 67,
        .ui8_target_battery_max_power_div25 = 20,    // 500W (500/25 = 20)
        .ui8_optional_ADC_function = 0,               // 0 = no function
        .ui8_torque_smooth_min = 255,                // If torque adc value < min -> smooth disabled
        .ui8_torque_smooth_max = 0,                  // If torque adc delta value > max -> smooth disabled
        .ui8_torque_smooth_enabled = 0
        };

// system
static uint8_t ui8_riding_mode = OFF_MODE;
static uint8_t ui8_riding_mode_parameter = 0;
static uint8_t ui8_walk_assist_parameter = 0;
static uint8_t ui8_motor_enabled = 1;
static uint8_t ui8_assist_without_pedal_rotation_threshold = 0;
static uint8_t ui8_assist_without_pedal_rotation_enabled = 0;
static uint8_t ui8_lights_configuration = 0;
static uint8_t ui8_lights_state = 0;
static uint8_t ui8_walk_assist = 0;
static uint8_t ui8_cruise_enabled = 0;
static uint8_t ui8_assist_whit_error_enabled = 0;

// power control
static uint8_t ui8_battery_current_max = DEFAULT_VALUE_BATTERY_CURRENT_MAX;
static uint8_t ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
static uint8_t ui8_duty_cycle_ramp_up_inverse_step_default = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
static uint8_t ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
static uint8_t ui8_duty_cycle_ramp_down_inverse_step_default = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
static uint8_t ui8_duty_cycle_ramp_up_inverse_step_after_braking = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
static uint8_t ui8_motor_acceleration_delay_after_brake = 0;
static uint16_t ui16_adc_battery_voltage_filtered = 0;
static uint16_t ui16_battery_voltage_filtered_x1000 = 0;
static uint8_t ui8_adc_battery_current_max = ADC_10_BIT_BATTERY_CURRENT_MAX;
static uint8_t ui8_adc_battery_current_target = 0;
static uint8_t ui8_duty_cycle_target = 0;
static uint8_t ui8_hall_ref_angles_config[6];

// acceleration after braking smoothing
static uint8_t ui8_brake_previously_set = 0;

// cadence sensor
uint16_t ui16_cadence_ticks_count_min_speed_adj = CADENCE_SENSOR_CALC_COUNTER_MIN;
static uint8_t ui8_pedal_cadence_RPM = 0;
volatile uint8_t ui8_brake_fast_stop = 0;

// torque sensor
// static uint8_t toffset_cycle_counter = 0;
static uint16_t ui16_adc_pedal_torque_offset = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint16_t ui16_adc_pedal_torque_offset_fix = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint16_t ui16_adc_pedal_torque_offset_init = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint16_t ui16_adc_pedal_torque_offset_min = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint16_t ui16_adc_pedal_torque_offset_max = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint16_t ui16_adc_pedal_torque_offset_temp = ADC_TORQUE_SENSOR_OFFSET_DEFAULT;
static uint8_t ui8_adc_pedal_torque_offset_error = 0;
static uint16_t ui16_adc_pedal_torque_range = 0;
uint16_t ui16_adc_coaster_brake_threshold = 0;
static uint8_t ui8_coaster_brake_enabled = 0;
static uint8_t ui8_coaster_brake_torque_threshold = 0;
static uint16_t ui16_adc_pedal_torque = 0;
static uint16_t ui16_adc_pedal_torque_delta = 0;
static uint16_t ui16_adc_pedal_torque_power_mode = 0;
static uint8_t ui8_torque_sensor_calibration_enabled = 0;
static uint8_t ui8_hybrid_torque_parameter = 0;
static uint8_t ui8_torque_sensor_filter_value = 0;
static uint8_t ui16_adc_torque_filtered = 0;

// wheel speed sensor
static uint16_t ui16_wheel_speed_x10 = 0;
static uint16_t ui16_wheel_calc_const;

// throttle control
volatile uint8_t ui8_throttle_adc = 0;
volatile uint8_t ui8_throttle_virtual;

// motor temperature control
static uint16_t ui16_adc_motor_temperature_filtered = 0;
static uint16_t ui8_motor_temperature_filtered = 0;
static uint8_t ui8_motor_temperature_max_value_to_limit = 0;
static uint8_t ui8_motor_temperature_min_value_to_limit = 0;

// eMTB assist
#define eMTB_POWER_FUNCTION_ARRAY_SIZE      241

static const uint8_t ui8_eMTB_power_function_160[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5,
        5, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12,
        12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18,
        19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 26, 26, 26, 27,
        27, 27, 27, 28, 28, 28, 29, 29, 29, 30, 30, 30, 31, 31, 31, 32, 32, 32, 33, 33, 33, 34, 34, 34, 35, 35, 35, 36,
        36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 40, 40, 40, 41, 41, 41, 42, 42, 42, 43, 43, 44, 44, 44, 45, 45, 45, 46,
        46, 47, 47, 47, 48, 48, 48, 49, 49, 50, 50, 50, 51, 51, 52, 52, 52, 53, 53, 54, 54, 54, 55, 55, 56, 56, 56, 57,
        57, 58, 58, 58, 59, 59, 60, 60, 61, 61, 61, 62, 62, 63, 63, 63, 64, 64 };
static const uint8_t ui8_eMTB_power_function_165[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6, 6,
        6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 14,
        14, 14, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 20, 21, 21, 21, 22, 22, 22, 23,
        23, 23, 24, 24, 24, 25, 25, 25, 26, 26, 27, 27, 27, 28, 28, 28, 29, 29, 30, 30, 30, 31, 31, 32, 32, 32, 33, 33,
        34, 34, 34, 35, 35, 36, 36, 36, 37, 37, 38, 38, 39, 39, 39, 40, 40, 41, 41, 42, 42, 42, 43, 43, 44, 44, 45, 45,
        46, 46, 47, 47, 47, 48, 48, 49, 49, 50, 50, 51, 51, 52, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59,
        59, 60, 60, 61, 61, 62, 62, 63, 63, 64, 64, 65, 65, 66, 66, 67, 67, 68, 68, 69, 69, 70, 71, 71, 72, 72, 73, 73,
        74, 74, 75, 75, 76, 77, 77, 78, 78, 79, 79, 80, 81, 81, 82, 82, 83, 83, 84, 85 };
static const uint8_t ui8_eMTB_power_function_170[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7, 7,
        7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 15, 16, 16, 16,
        17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 21, 21, 21, 22, 22, 23, 23, 23, 24, 24, 25, 25, 26, 26, 26, 27, 27, 28,
        28, 29, 29, 30, 30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41,
        42, 42, 43, 43, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 50, 51, 51, 52, 52, 53, 53, 54, 55, 55, 56, 56, 57,
        58, 58, 59, 59, 60, 61, 61, 62, 63, 63, 64, 64, 65, 66, 66, 67, 68, 68, 69, 70, 70, 71, 71, 72, 73, 73, 74, 75,
        75, 76, 77, 77, 78, 79, 80, 80, 81, 82, 82, 83, 84, 84, 85, 86, 87, 87, 88, 89, 89, 90, 91, 92, 92, 93, 94, 94,
        95, 96, 97, 97, 98, 99, 100, 100, 101, 102, 103, 103, 104, 105, 106, 107, 107, 108, 109, 110, 110, 111 };
static const uint8_t ui8_eMTB_power_function_175[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
        1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9,
        9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 16, 16, 17, 17, 17, 18, 18, 19, 19, 20,
        20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 31, 31, 32, 32, 33, 33, 34,
        34, 35, 36, 36, 37, 37, 38, 39, 39, 40, 40, 41, 42, 42, 43, 44, 44, 45, 45, 46, 47, 47, 48, 49, 49, 50, 51, 51,
        52, 53, 53, 54, 55, 56, 56, 57, 58, 58, 59, 60, 61, 61, 62, 63, 64, 64, 65, 66, 67, 67, 68, 69, 70, 70, 71, 72,
        73, 74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 82, 83, 83, 84, 85, 86, 87, 88, 88, 89, 90, 91, 92, 93, 94, 95, 95,
        96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
        118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
        140, 141, 142, 143, 144, 145, 146 };
static const uint8_t ui8_eMTB_power_function_180[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
        1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10,
        11, 11, 11, 12, 12, 13, 13, 14, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24,
        24, 25, 25, 26, 27, 27, 28, 28, 29, 30, 30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 38, 38, 39, 40, 41, 41, 42,
        43, 43, 44, 45, 46, 46, 47, 48, 49, 50, 50, 51, 52, 53, 54, 54, 55, 56, 57, 58, 59, 59, 60, 61, 62, 63, 64, 65,
        66, 67, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92,
        93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 105, 106, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117, 118,
        119, 120, 122, 123, 124, 125, 126, 128, 129, 130, 131, 132, 134, 135, 136, 137, 139, 140, 141, 142, 144, 145,
        146, 147, 149, 150, 151, 153, 154, 155, 157, 158, 159, 161, 162, 163, 165, 166, 167, 169, 170, 171, 173, 174,
        176, 177, 178, 180, 181, 182, 184, 185, 187, 188, 190, 191, 192 };
static const uint8_t ui8_eMTB_power_function_185[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
        1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 11, 12,
        12, 13, 13, 14, 14, 15, 15, 16, 17, 17, 18, 18, 19, 19, 20, 21, 21, 22, 23, 23, 24, 25, 25, 26, 27, 27, 28, 29,
        29, 30, 31, 32, 32, 33, 34, 35, 36, 36, 37, 38, 39, 40, 40, 41, 42, 43, 44, 45, 46, 46, 47, 48, 49, 50, 51, 52,
        53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 74, 75, 76, 77, 78, 79, 80, 81,
        83, 84, 85, 86, 87, 89, 90, 91, 92, 93, 95, 96, 97, 98, 100, 101, 102, 104, 105, 106, 107, 109, 110, 111, 113,
        114, 115, 117, 118, 120, 121, 122, 124, 125, 127, 128, 129, 131, 132, 134, 135, 137, 138, 140, 141, 143, 144,
        146, 147, 149, 150, 152, 153, 155, 156, 158, 160, 161, 163, 164, 166, 168, 169, 171, 172, 174, 176, 177, 179,
        181, 182, 184, 186, 187, 189, 191, 193, 194, 196, 198, 199, 201, 203, 205, 207, 208, 210, 212, 214, 216, 217,
        219, 221, 223, 225, 227, 228, 230, 232, 234, 236, 238, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_190[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
        1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14,
        14, 15, 16, 16, 17, 18, 18, 19, 20, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31, 32, 33, 34, 35,
        36, 37, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56, 57, 58, 60, 61, 62, 63, 64,
        66, 67, 68, 69, 70, 72, 73, 74, 76, 77, 78, 80, 81, 82, 84, 85, 86, 88, 89, 91, 92, 94, 95, 96, 98, 99, 101,
        102, 104, 105, 107, 108, 110, 112, 113, 115, 116, 118, 120, 121, 123, 124, 126, 128, 130, 131, 133, 135, 136,
        138, 140, 142, 143, 145, 147, 149, 150, 152, 154, 156, 158, 160, 162, 163, 165, 167, 169, 171, 173, 175, 177,
        179, 181, 183, 185, 187, 189, 191, 193, 195, 197, 199, 201, 203, 205, 207, 209, 211, 214, 216, 218, 220, 222,
        224, 227, 229, 231, 233, 235, 238, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240 };
static const uint8_t ui8_eMTB_power_function_195[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
        1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 13, 13, 14, 15, 15, 16,
        17, 17, 18, 19, 20, 21, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 39, 40, 41, 42,
        43, 44, 45, 47, 48, 49, 50, 51, 53, 54, 55, 57, 58, 59, 61, 62, 63, 65, 66, 68, 69, 70, 72, 73, 75, 76, 78, 79,
        81, 83, 84, 86, 87, 89, 91, 92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110, 112, 113, 115, 117, 119, 121, 123,
        125, 127, 129, 131, 132, 134, 136, 139, 141, 143, 145, 147, 149, 151, 153, 155, 157, 160, 162, 164, 166, 168,
        171, 173, 175, 177, 180, 182, 184, 187, 189, 191, 194, 196, 199, 201, 203, 206, 208, 211, 213, 216, 218, 221,
        224, 226, 229, 231, 234, 237, 239, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_200[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
        1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 10, 10, 11, 12, 12, 13, 14, 14, 15, 16, 17, 18, 18, 19,
        20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 40, 41, 42, 44, 45, 46, 48, 49, 50, 52,
        53, 55, 56, 58, 59, 61, 62, 64, 66, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 85, 86, 88, 90, 92, 94, 96, 98, 100,
        102, 104, 106, 108, 110, 112, 114, 117, 119, 121, 123, 125, 128, 130, 132, 135, 137, 139, 142, 144, 146, 149,
        151, 154, 156, 159, 161, 164, 166, 169, 172, 174, 177, 180, 182, 185, 188, 190, 193, 196, 199, 202, 204, 207,
        210, 213, 216, 219, 222, 225, 228, 231, 234, 237, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_205[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
        2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 9, 9, 10, 11, 11, 12, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 26, 27, 28, 29, 30, 32, 33, 34, 36, 37, 38, 40, 41, 43, 44, 46, 47, 49, 50, 52, 54, 55, 57, 59, 61, 62,
        64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 95, 97, 99, 101, 104, 106, 108, 111, 113, 116, 118,
        121, 123, 126, 128, 131, 134, 136, 139, 142, 145, 147, 150, 153, 156, 159, 162, 165, 168, 171, 174, 177, 180,
        183, 186, 189, 192, 196, 199, 202, 205, 209, 212, 216, 219, 222, 226, 229, 233, 236, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_210[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
        2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 26, 27,
        28, 30, 31, 32, 34, 35, 37, 39, 40, 42, 43, 45, 47, 49, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 71, 73, 75, 77,
        80, 82, 84, 87, 89, 92, 94, 97, 99, 102, 104, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 136, 139, 142,
        145, 149, 152, 155, 158, 162, 165, 169, 172, 176, 179, 183, 186, 190, 194, 197, 201, 205, 209, 213, 216, 220,
        224, 228, 232, 237, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_215[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
        2, 2, 3, 3, 4, 4, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 24, 25, 26, 28, 29, 31,
        33, 34, 36, 38, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 60, 62, 64, 67, 69, 71, 74, 76, 79, 82, 84, 87, 90, 93,
        96, 98, 101, 104, 107, 111, 114, 117, 120, 123, 127, 130, 134, 137, 141, 144, 148, 152, 155, 159, 163, 167, 171,
        175, 179, 183, 187, 191, 195, 200, 204, 208, 213, 217, 222, 226, 231, 235, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_220[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
        2, 3, 3, 4, 4, 5, 6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 22, 23, 25, 27, 28, 30, 32, 33, 35, 37,
        39, 41, 43, 46, 48, 50, 52, 55, 57, 60, 62, 65, 67, 70, 73, 76, 79, 82, 85, 88, 91, 94, 97, 101, 104, 108, 111,
        115, 118, 122, 126, 130, 133, 137, 141, 145, 150, 154, 158, 162, 167, 171, 176, 180, 185, 190, 194, 199, 204,
        209, 214, 219, 224, 230, 235, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_225[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
        3, 3, 4, 4, 5, 6, 7, 8, 8, 9, 10, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24, 26, 28, 30, 32, 34, 36, 38, 40, 43,
        45, 47, 50, 52, 55, 58, 61, 64, 66, 70, 73, 76, 79, 82, 86, 89, 93, 96, 100, 104, 108, 112, 116, 120, 124, 128,
        133, 137, 142, 146, 151, 156, 161, 166, 171, 176, 181, 186, 191, 197, 202, 208, 214, 219, 225, 231, 237, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_230[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2,
        3, 4, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 18, 20, 21, 23, 25, 27, 29, 31, 33, 36, 38, 40, 43, 46, 48, 51,
        54, 57, 60, 63, 67, 70, 74, 77, 81, 85, 88, 92, 96, 101, 105, 109, 114, 118, 123, 128, 133, 138, 143, 148, 153,
        158, 164, 170, 175, 181, 187, 193, 199, 205, 212, 218, 225, 231, 238, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_235[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3,
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 37, 40, 43, 45, 48, 52, 55, 58, 62,
        65, 69, 73, 77, 81, 85, 89, 94, 98, 103, 108, 113, 118, 123, 128, 134, 139, 145, 151, 157, 163, 169, 176, 182,
        189, 196, 202, 210, 217, 224, 232, 239, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_240[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3,
        4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 19, 21, 23, 25, 27, 30, 32, 35, 38, 41, 44, 47, 51, 54, 58, 62, 66, 70,
        74, 79, 83, 88, 93, 98, 103, 108, 114, 120, 125, 131, 137, 144, 150, 157, 164, 171, 178, 185, 193, 200, 208,
        216, 224, 233, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240 };
static const uint8_t ui8_eMTB_power_function_245[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4,
        4, 5, 6, 8, 9, 10, 12, 14, 15, 17, 19, 22, 24, 27, 29, 32, 35, 38, 42, 45, 49, 53, 57, 61, 65, 70, 74, 79, 84,
        89, 95, 100, 106, 112, 119, 125, 132, 138, 145, 153, 160, 168, 176, 184, 192, 200, 209, 218, 227, 237, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240 };
static const uint8_t ui8_eMTB_power_function_250[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4,
        5, 6, 7, 9, 10, 12, 14, 16, 18, 20, 23, 25, 28, 31, 34, 38, 41, 45, 49, 54, 58, 63, 67, 72, 78, 83, 89, 95, 101,
        108, 114, 121, 128, 136, 144, 151, 160, 168, 177, 186, 195, 204, 214, 224, 235, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240 };
static const uint8_t ui8_eMTB_power_function_255[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 4, 5,
        6, 7, 8, 10, 12, 14, 16, 18, 21, 24, 26, 30, 33, 37, 41, 45, 49, 54, 58, 64, 69, 75, 80, 87, 93, 100, 107, 114,
        122, 130, 138, 146, 155, 164, 174, 184, 194, 204, 215, 226, 238, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
        240, 240, 240 };

// cruise
static int16_t i16_cruise_pid_kp = 0;
static int16_t i16_cruise_pid_ki = 0;
static uint8_t ui8_cruise_PID_initialize = 1;
static uint16_t ui16_wheel_speed_target_received_x10 = 0;

// startup boost
static uint8_t ui8_startup_boost_enabled = 0;
static uint8_t ui8_startup_boost_factor_array[120];
static uint8_t ui8_startup_boost_cadence_step = 0;

// UART
#define UART_RECEIVE_RINGBUFFER_SIZE   256 // Maximum size - can reduce later
#define UART_NUMBER_DATA_BYTES_TO_RECEIVE   45
#define UART_NUMBER_DATA_BYTES_TO_SEND      30
volatile uint8_t ui8_rx_ringbuffer[UART_RECEIVE_RINGBUFFER_SIZE];
volatile uint8_t ui8_rx_ringbuffer_read_index = 0;
volatile uint8_t ui8_rx_ringbuffer_write_index = 0;
volatile uint8_t ui8_received_package_flag = 0;
volatile uint8_t ui8_rx_buffer[UART_NUMBER_DATA_BYTES_TO_RECEIVE];
volatile uint8_t ui8_rx_cnt = 0;
volatile uint8_t ui8_rx_len = 0;
volatile uint8_t ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND];
static volatile uint8_t ui8_m_tx_buffer_index;
volatile uint8_t ui8_packet_len;
volatile uint8_t ui8_i;
volatile uint8_t ui8_byte_received;
volatile uint8_t ui8_state_machine = 0;
static uint16_t ui16_crc_rx;
static uint16_t ui16_crc_tx;
static uint8_t ui8_comm_error_counter = 0;

// communications functions
void communications_controller(void);
static void communications_process_packages(uint8_t ui8_frame_type);
static void packet_assembler(void);

// system functions
static void calc_motor_erps(void);
static void get_battery_voltage(void);
static void get_pedal_torque(void);
static void calc_wheel_speed(void);
static void calc_cadence(void);

static void ebike_control_lights(void);
static void ebike_control_motor(void);
static void check_system(void);

static void apply_power_assist();
static void apply_cadence_assist();
static void apply_torque_assist();
static void apply_emtb_assist();
static void apply_hybrid_assist();
static void apply_cruise();
static void apply_walk_assist();
static void apply_throttle();
static void apply_pwm_calibration_assist();
static void apply_erps_calibration_assist();
static void apply_temperature_limiting();
static void apply_speed_limit();
static void set_motor_acceleration();

void ebike_app_controller(void) {

    // get motor erps
    calc_motor_erps();

    // calculate the wheel speed
    calc_wheel_speed();

    // calculate the cadence and set limits from wheel speed
    calc_cadence();
    
    // get pedal torque
    get_pedal_torque();

    // get battery voltage
    get_battery_voltage();

    // check if there are any errors for motor control
    check_system();

    // use previously received data and sensor input to control motor
    ebike_control_motor();
    
    // assemble packets
    packet_assembler();
    // communicate with display
    communications_controller();

    // use received data to control external lights  
    ebike_control_lights(); 

    /*------------------------------------------------------------------------

     NOTE: regarding function call order

     Do not change order of functions if not absolutely sure it will
     not cause any undesirable consequences.

     ------------------------------------------------------------------------*/
}

static void ebike_control_motor(void) {
    // reset control variables (safety)
    ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
    ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
    ui8_adc_battery_current_target = 0;
    ui8_duty_cycle_target = 0;

    // set brake_previously_set flag
    if (ui8_brake_state) {
        ui8_brake_previously_set = ui8_motor_acceleration_delay_after_brake;        
    } else if (ui8_brake_previously_set) {
        ui8_brake_previously_set--;
    }

    // reset initialization of Cruise PID controller
    if (ui8_riding_mode != CRUISE_MODE) {
        ui8_cruise_PID_initialize = 1;
    }

    // select riding mode
    switch (ui8_riding_mode) {
        case POWER_ASSIST_MODE: apply_power_assist(); break;
		case TORQUE_ASSIST_MODE: apply_torque_assist(); break;
		case CADENCE_ASSIST_MODE: apply_cadence_assist(); break;
		case eMTB_ASSIST_MODE: apply_emtb_assist(); break;
		case HYBRID_ASSIST_MODE: apply_hybrid_assist(); break;
		case CRUISE_MODE: apply_cruise(); break;
		case WALK_ASSIST_MODE: apply_walk_assist(); break;
        case PWM_CALIBRATION_ASSIST_MODE: apply_pwm_calibration_assist(); break;
        case ERPS_CALIBRATION_ASSIST_MODE: apply_erps_calibration_assist(); break;
    }

    // select optional ADC function
    switch (m_configuration_variables.ui8_optional_ADC_function) {
        case THROTTLE_CONTROL:
			apply_throttle();
			break;
		case TEMPERATURE_CONTROL:
			apply_temperature_limiting();
			if (ui8_throttle_virtual) {apply_throttle();}
			break;
		default:
			if (ui8_throttle_virtual) {apply_throttle();}
			break;
    }

    // speed limit
    apply_speed_limit();

    // reset control parameters if... (safety)
    if (ui8_brake_state || ui8_m_system_state & 8 || ui8_m_system_state & 32 || !ui8_motor_enabled) {
        ui8_controller_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
        ui8_controller_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        ui8_controller_adc_battery_current_target = 0;
        ui8_controller_duty_cycle_target = 0;
    } else {
        // limit max current if higher than configured hardware limit (safety)
        if (ui8_adc_battery_current_max > ADC_10_BIT_BATTERY_CURRENT_MAX) {
            ui8_adc_battery_current_max = ADC_10_BIT_BATTERY_CURRENT_MAX;
        }

        // limit target current if higher than max value (safety)
        if (ui8_adc_battery_current_target > ui8_adc_battery_current_max) {
            ui8_adc_battery_current_target = ui8_adc_battery_current_max;
        }

        // limit target duty cycle if higher than max value
        if (ui8_duty_cycle_target > PWM_DUTY_CYCLE_MAX) {
            ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
        }

        // limit target duty cycle ramp up inverse step if lower than min value (safety)
        if (ui8_duty_cycle_ramp_up_inverse_step < PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
        }

        // limit target duty cycle ramp down inverse step if lower than min value (safety)
        if (ui8_duty_cycle_ramp_down_inverse_step < PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN) {
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        }

        // set duty cycle ramp up in controller
        ui8_controller_duty_cycle_ramp_up_inverse_step = ui8_duty_cycle_ramp_up_inverse_step;

        // set duty cycle ramp down in controller
        ui8_controller_duty_cycle_ramp_down_inverse_step = ui8_duty_cycle_ramp_down_inverse_step;

        // set target battery current in controller
        ui8_controller_adc_battery_current_target = ui8_adc_battery_current_target;

        // set target duty cycle in controller
        ui8_controller_duty_cycle_target = ui8_duty_cycle_target;
    }

    switch (ui8_m_motor_init_state)
	{
    case MOTOR_INIT_STATE_INIT_START_DELAY:
      m_ui8_got_configurations_timer = 40;
      ui8_m_motor_init_state = MOTOR_INIT_STATE_INIT_WAIT_DELAY;
      // no break to execute next code

    case MOTOR_INIT_STATE_INIT_WAIT_DELAY:
      if (m_ui8_got_configurations_timer > 0) {
        m_ui8_got_configurations_timer--;
      }
      else
      {
        ui8_m_motor_init_state = MOTOR_INIT_OK;
        ui8_m_motor_init_status = MOTOR_INIT_STATUS_INIT_OK;
        ui8_m_system_state &= ~ERROR_NOT_INIT;
	  }
	break;
	}

    // check if the motor should be enabled or disabled
    if (ui8_motor_enabled
            && (ui16_motor_speed_erps == 0)
            && (!ui8_adc_battery_current_target)
            && (!ui8_g_duty_cycle)) {
        ui8_motor_enabled = 0;
        motor_disable_pwm();
    } else if (!ui8_motor_enabled
            && (ui16_motor_speed_erps < 30) // enable the motor only if it rotates slowly or is stopped
            && (ui8_adc_battery_current_target)
            && (!ui8_brake_state)) {
        ui8_motor_enabled = 1;
        ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        ui8_g_duty_cycle = PWM_DUTY_CYCLE_STARTUP;
        ui8_fw_hall_counter_offset = 0;
        motor_enable_pwm();
    }
}

static void set_motor_acceleration() {

    uint8_t ui8_tmp = 0;
    uint8_t ui8_duty_cycle_ramp_up_inverse_step_adjusted;
    
    // set adjusted rates dependong on if brake has been recently set
    if (ui8_brake_previously_set) {
        ui8_duty_cycle_ramp_up_inverse_step_adjusted = ui8_duty_cycle_ramp_up_inverse_step_after_braking;
    } else {
        ui8_duty_cycle_ramp_up_inverse_step_adjusted = ui8_duty_cycle_ramp_up_inverse_step_default;
    }
    
    // set motor acceleration
    if (ui16_wheel_speed_x10 >= 240) {
        ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
    } else {
        ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
            (uint8_t)10, // 10*4 = 40 -> 4 kph
            (uint8_t)60, // 60*4 = 240 -> 24 kph
            (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_adjusted,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
        ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
            (uint8_t)20, // 20 rpm
            (uint8_t)80, // 80 rpm
            (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_adjusted,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
        if (ui8_tmp < ui8_duty_cycle_ramp_up_inverse_step) {
            ui8_duty_cycle_ramp_up_inverse_step = ui8_tmp;    
        }

        ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
            (uint8_t)10, // 10*4 = 40 -> 4 kph
            (uint8_t)60, // 60*4 = 240 -> 24 kph
            (uint8_t)ui8_duty_cycle_ramp_down_inverse_step_default,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
            (uint8_t)20, // 20 rpm
            (uint8_t)80, // 80 rpm
            (uint8_t)ui8_duty_cycle_ramp_down_inverse_step_default,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        if (ui8_tmp < ui8_duty_cycle_ramp_down_inverse_step) {
            ui8_duty_cycle_ramp_down_inverse_step = ui8_tmp;
        }       
    }
}

static void apply_power_assist()
{
    uint8_t ui8_power_assist_multiplier_x50 = ui8_riding_mode_parameter;

    // pedal torque delta + startup boost in power assist mode
	if(ui8_startup_boost_enabled && (ui8_pedal_cadence_RPM < 120)) {
		// calculate startup boost torque & new pedal torque delta
		uint16_t ui16_temp = (uint16_t)((uint16_t)((uint8_t)ui16_adc_pedal_torque_delta * ui8_startup_boost_factor_array[ui8_pedal_cadence_RPM]) / 50);
		ui16_adc_pedal_torque_power_mode = ui16_temp + ui16_adc_pedal_torque_delta;
	} else {
        ui16_adc_pedal_torque_power_mode = ui16_adc_pedal_torque_delta;
    }

	// check for assist without pedal rotation when there is no pedal rotation	
    if(ui8_assist_without_pedal_rotation_enabled) {
		if((ui8_pedal_cadence_RPM < 4) && (ui16_adc_pedal_torque_delta > ui8_assist_without_pedal_rotation_threshold)) {
				ui8_pedal_cadence_RPM = 4;
		}
	}
        /*------------------------------------------------------------------------
         NOTE: regarding the human power calculation

         (1) Formula: pedal power = torque * rotations per second * 2 * pi
         (2) Formula: pedal power = torque * rotations per minute * 2 * pi / 60
         (3) Formula: pedal power = torque * rotations per minute * 0.1047
         (4) Formula: pedal power = torque * 100 * rotations per minute * 0.001047
         (5) Formula: pedal power = torque * 100 * rotations per minute / 955
         (6) Formula: pedal power * 100  =  torque * 100 * rotations per minute * (100 / 955)
         (7) Formula: assist power * 100  =  torque * 100 * rotations per minute * (100 / 955) * (ui8_power_assist_multiplier_x50 / 50)
         (8) Formula: assist power * 100  =  torque * 100 * rotations per minute * (2 / 955) * ui8_power_assist_multiplier_x50
         (9) Formula: assist power * 100  =  torque * 100 * rotations per minute * ui8_power_assist_multiplier_x50 / 480
         ------------------------------------------------------------------------*/

	// calculate torque on pedals + torque startup boost
    uint32_t ui32_pedal_torque_x100 = (uint32_t)(ui16_adc_pedal_torque_power_mode * m_configuration_variables.ui8_pedal_torque_per_10_bit_ADC_step_x100);

    // calculate power assist by multiplying human power with the power assist multiplier
    uint32_t ui32_power_assist_x100 = (((uint32_t)(ui8_pedal_cadence_RPM * ui8_power_assist_multiplier_x50)) * ui32_pedal_torque_x100); // eliminate divide by 480U - saves __divulong function call

    // Battery current target (Amps x 100) - // change multiply by 1000 to multiply by 2 (1000/480 is approx. 2 - difference is 4% and can be fixed with pedal_torque_ADC_step variable) - saves __mullong function call
    uint16_t ui16_battery_current_target_x100 = (uint16_t)((ui32_power_assist_x100 * 2) / ui16_battery_voltage_filtered_x1000); 

    // Battery current target (ADC steps)
    uint16_t ui16_adc_battery_current_target = ui16_battery_current_target_x100 / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;

    // set ramps
    set_motor_acceleration();

    // set battery current target
    if (ui16_adc_battery_current_target > ui8_adc_battery_current_max) {
        ui8_adc_battery_current_target = ui8_adc_battery_current_max;
    } else {
        ui8_adc_battery_current_target = ui16_adc_battery_current_target;
    }

    // set duty cycle target
    if (ui8_adc_battery_current_target) {
        ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
    } else {
        ui8_duty_cycle_target = 0;
    }
}

static void apply_torque_assist()
{
    // check for assist without pedal rotation threshold when there is no pedal rotation and standing still
    if(ui8_assist_without_pedal_rotation_enabled) {
        if (!ui8_pedal_cadence_RPM && (ui16_adc_pedal_torque_delta > ui8_assist_without_pedal_rotation_threshold)) {
            ui8_pedal_cadence_RPM = 1;
        }    
    }

    // calculate torque assistance
    if (ui16_adc_pedal_torque_delta && ui8_pedal_cadence_RPM) {
        // get the torque assist factor
        uint8_t ui8_torque_assist_factor = ui8_riding_mode_parameter;

        // calculate torque assist target current
        uint16_t ui16_adc_battery_current_target_torque_assist = ((uint16_t) ui16_adc_pedal_torque_delta * ui8_torque_assist_factor) / TORQUE_ASSIST_FACTOR_DENOMINATOR;

        // set ramps
        set_motor_acceleration();

        // set battery current target
        if (ui16_adc_battery_current_target_torque_assist > ui8_adc_battery_current_max) {
            ui8_adc_battery_current_target = ui8_adc_battery_current_max;
        } else {
            ui8_adc_battery_current_target = ui16_adc_battery_current_target_torque_assist;
        }

        // set duty cycle target
        if (ui8_adc_battery_current_target) {
            ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
        } else {
            ui8_duty_cycle_target = 0;
        }
    }
}

static void apply_cadence_assist()
{
    if (ui8_pedal_cadence_RPM) {
        // get the cadence assist duty cycle target
		uint8_t ui8_cadence_assist_duty_cycle_target = ui8_riding_mode_parameter + ui8_pedal_cadence_RPM;

        // limit cadence assist duty cycle target
        if (ui8_cadence_assist_duty_cycle_target > PWM_DUTY_CYCLE_MAX) {
            ui8_cadence_assist_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
        }

        // set motor acceleration
        if (ui16_wheel_speed_x10 >= 240) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        } else {
            ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
            (uint8_t)10, // 10*4 = 40 -> 4 kph
            (uint8_t)60, // 60*4 = 240 -> 24 kph
            (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_default + PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_CADENCE_OFFSET,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
            ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
            (uint8_t)10, // 10*4 = 40 -> 4 kph
            (uint8_t)60, // 60*4 = 240 -> 24 kph
            (uint8_t)ui8_duty_cycle_ramp_down_inverse_step_default,
            (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        }
        // set adjusted ramp-up if brake has been recently set - values from display
        if (ui8_brake_previously_set) {
            ui8_duty_cycle_ramp_up_inverse_step = ui8_duty_cycle_ramp_up_inverse_step_after_braking;
        }

        // set battery current target
        ui8_adc_battery_current_target = ui8_adc_battery_current_max;

        // set duty cycle target
        ui8_duty_cycle_target = ui8_cadence_assist_duty_cycle_target;
    }
}

static void apply_emtb_assist() 
{
    #define eMTB_ASSIST_ADC_TORQUE_OFFSET    10

    // check for assist without pedal rotation threshold when there is no pedal rotation and standing still
    if(ui8_assist_without_pedal_rotation_enabled) {
        if (!ui8_pedal_cadence_RPM && (ui16_adc_pedal_torque_delta > ui8_assist_without_pedal_rotation_threshold)) {
            ui8_pedal_cadence_RPM = 1;
        }    
    }

    if ((ui16_adc_pedal_torque_delta > 0)
            && (ui16_adc_pedal_torque_delta < (eMTB_POWER_FUNCTION_ARRAY_SIZE - eMTB_ASSIST_ADC_TORQUE_OFFSET))
            && (ui8_pedal_cadence_RPM)) {
        // initialize eMTB assist target current
        uint8_t ui8_adc_battery_current_target_eMTB_assist = 0;

        // get the eMTB assist sensitivity
        uint8_t ui8_eMTB_assist_sensitivity = ui8_riding_mode_parameter;

        switch (ui8_eMTB_assist_sensitivity) {
        case 1:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_160[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 2:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_165[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 3:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_170[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 4:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_175[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 5:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_180[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 6:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_185[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 7:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_190[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 8:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_195[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 9:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_200[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 10:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_205[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 11:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_210[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 12:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_215[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 13:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_220[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 14:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_225[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 15:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_230[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 16:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_235[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 17:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_240[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 18:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_245[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 19:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_250[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        case 20:
            ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_255[ui16_adc_pedal_torque_delta
                    + eMTB_ASSIST_ADC_TORQUE_OFFSET];
            break;
        }

        // set ramps
        set_motor_acceleration();

        // set battery current target
        if (ui8_adc_battery_current_target_eMTB_assist > ui8_adc_battery_current_max) {
            ui8_adc_battery_current_target = ui8_adc_battery_current_max;
        } else {
            ui8_adc_battery_current_target = ui8_adc_battery_current_target_eMTB_assist;
        }

        // set duty cycle target
        if (ui8_adc_battery_current_target) {
            ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
        } else {
            ui8_duty_cycle_target = 0;
        }
    }
}

static void apply_hybrid_assist()
{		
	uint16_t ui16_adc_battery_current_target_power_assist;
	uint16_t ui16_adc_battery_current_target_torque_assist;
	static uint16_t ui16_adc_battery_current_target;

    // get the torque assist factor
	uint8_t ui8_torque_assist_factor = ui8_hybrid_torque_parameter;
    // get the power assist multiplier
	uint8_t ui8_power_assist_multiplier_x50 = ui8_riding_mode_parameter;

	// check for assist without pedal rotation when there is no pedal rotation
	if(ui8_assist_without_pedal_rotation_enabled) {
        if (!ui8_pedal_cadence_RPM && (ui16_adc_pedal_torque_delta > ui8_assist_without_pedal_rotation_threshold)) {
            ui8_pedal_cadence_RPM = 1;
        }    
    }

	// calculate torque assistance
	if (ui16_adc_pedal_torque_delta && ui8_pedal_cadence_RPM)
	{
		// calculate torque assist target current
		ui16_adc_battery_current_target_torque_assist = ((uint16_t) ui16_adc_pedal_torque_delta * ui8_torque_assist_factor) / TORQUE_ASSIST_FACTOR_DENOMINATOR;
	} else {
		ui16_adc_battery_current_target_torque_assist = 0;
	}

	// calculate power assistance
	// calculate torque on pedals
    uint32_t ui32_pedal_torque_x100 = (uint32_t)(ui16_adc_pedal_torque_delta * m_configuration_variables.ui8_pedal_torque_per_10_bit_ADC_step_x100);

    // calculate power assist by multiplying human power with the power assist multiplier
    uint32_t ui32_power_assist_x100 = (((uint32_t)(ui8_pedal_cadence_RPM * ui8_power_assist_multiplier_x50)) * ui32_pedal_torque_x100); // eliminate divide by 480U - saves __divulong function call

    // Battery current target (Amps x 100) - // change multiply by 1000 to multiply by 2 (1000/480 can be approximated to 2 - result is 4% less and can be fixed with pedal_torque_ADC_step variable) - saves __mullong function call
    uint16_t ui16_battery_current_target_x100 = (uint16_t)((ui32_power_assist_x100 * 2) / ui16_battery_voltage_filtered_x1000); 

    // Battery current target (ADC steps)
    ui16_adc_battery_current_target_power_assist = ui16_battery_current_target_x100 / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;

	// set battery current target in ADC steps and average with previous
	if(ui16_adc_battery_current_target_power_assist > ui16_adc_battery_current_target_torque_assist) {
		ui16_adc_battery_current_target = (ui16_adc_battery_current_target + ui16_adc_battery_current_target_power_assist) >> 1;
    }
	else {
		ui16_adc_battery_current_target = (ui16_adc_battery_current_target + ui16_adc_battery_current_target_torque_assist) >> 1;
    }
    
	// set ramps
    set_motor_acceleration();

	// set battery current target
	if (ui16_adc_battery_current_target > ui8_adc_battery_current_max) { ui8_adc_battery_current_target = ui8_adc_battery_current_max; }
	else { ui8_adc_battery_current_target = ui16_adc_battery_current_target; }

	// set duty cycle target
	if (ui8_adc_battery_current_target) { ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX; }
	else { ui8_duty_cycle_target = 0; }
}

static void apply_walk_assist() 
{
    #define WALK_ASSIST_DUTY_CYCLE_MAX                      80
    #define WALK_ASSIST_ADC_BATTERY_CURRENT_MAX             80

    if (ui16_wheel_speed_x10 < WALK_ASSIST_THRESHOLD_SPEED_X10) {
        // get the walk assist duty cycle target
      uint8_t ui8_walk_assist_duty_cycle_target = ui8_walk_assist_parameter;

        // check so that walk assist level factor is not too large (too powerful), if it is -> limit the value
        if (ui8_walk_assist_duty_cycle_target > WALK_ASSIST_DUTY_CYCLE_MAX) {
            ui8_walk_assist_duty_cycle_target = WALK_ASSIST_DUTY_CYCLE_MAX;
        }

        // set motor acceleration
        ui8_duty_cycle_ramp_up_inverse_step = WALK_ASSIST_DUTY_CYCLE_RAMP_UP_INVERSE_STEP;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;

        // set battery current target
        ui8_adc_battery_current_target = ui8_min(WALK_ASSIST_ADC_BATTERY_CURRENT_MAX, ui8_adc_battery_current_max);

        // set duty cycle target
        ui8_duty_cycle_target = ui8_walk_assist_duty_cycle_target;
    }
}

static void apply_cruise() 
{
    #define CRUISE_PID_INTEGRAL_LIMIT                 1000
    #define CRUISE_PID_KD                             5

    if (ui16_wheel_speed_x10 > CRUISE_THRESHOLD_SPEED_X10) {
        static int16_t i16_error;
        static int16_t i16_last_error;
        static int16_t i16_integral;
        static int16_t i16_derivative;
        static int16_t i16_control_output;
        static uint16_t ui16_wheel_speed_target_x10;

        // initialize cruise PID controller
        if (ui8_cruise_PID_initialize) {
            ui8_cruise_PID_initialize = 0;

            // reset PID variables
            i16_error = 0;
            i16_last_error = 0;
            i16_integral = 320; // initialize integral to a value so the motor does not start from zero
            i16_derivative = 0;
            i16_control_output = 0;

            // set current wheel speed to maintain
            ui16_wheel_speed_target_x10 = ui16_wheel_speed_x10;
        }

        // calculate error
        i16_error = (ui16_wheel_speed_target_x10 - ui16_wheel_speed_x10);

        // calculate integral
        i16_integral = i16_integral + i16_error;

        // limit integral
        if (i16_integral > CRUISE_PID_INTEGRAL_LIMIT) {
            i16_integral = CRUISE_PID_INTEGRAL_LIMIT;
        } else if (i16_integral < 0) {
            i16_integral = 0;
        }

        // calculate derivative
        i16_derivative = i16_error - i16_last_error;

        // save error to last error
        i16_last_error = i16_error;

        // calculate control output ( output =  P I D )
        i16_control_output = (i16_cruise_pid_kp * i16_error) + (i16_cruise_pid_ki * i16_integral)
                + (CRUISE_PID_KD * i16_derivative);

        // limit control output to just positive values
        if (i16_control_output < 0) {
            i16_control_output = 0;
        }

        // limit control output to the maximum value
        if (i16_control_output > 1000) {
            i16_control_output = 1000;
        }

        // set motor acceleration
        ui8_duty_cycle_ramp_up_inverse_step = CRUISE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;

        // set battery current target
        ui8_adc_battery_current_target = ui8_adc_battery_current_max;

        // set duty cycle target  |  map the control output to an appropriate target PWM value
        ui8_duty_cycle_target = map_ui8((uint8_t) (i16_control_output >> 2),
                (uint8_t)0,                   // minimum control output from PID
                (uint8_t)250,                 // maximum control output from PID
                (uint8_t)0,                   // minimum duty cycle
                (uint8_t)(PWM_DUTY_CYCLE_MAX-1)); // maximum duty cycle
    }
}

static void apply_throttle() {

    // map value from 0 to 255
    ui8_throttle_adc = map_ui8(((uint8_t)(ui16_adc_throttle >> 2)),
            (uint8_t) ADC_THROTTLE_MIN_VALUE,
            (uint8_t) ADC_THROTTLE_MAX_VALUE,
            (uint8_t) 0,
            (uint8_t) 255);

    // map ADC throttle value from 0 to max battery current
    uint8_t ui8_adc_battery_current_target_throttle = map_ui8((uint8_t) ui8_throttle_adc,
            (uint8_t) 0,
            (uint8_t) 255,
            (uint8_t) 0,
            (uint8_t) ui8_adc_battery_current_max);

    if (ui8_adc_battery_current_target_throttle > ui8_adc_battery_current_target) {
        // set motor acceleration
        if (ui16_wheel_speed_x10 >= 255) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        } else {
            ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t) ui16_wheel_speed_x10,
                    (uint8_t) 40,
                    (uint8_t) 255,
                    (uint8_t) THROTTLE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT,
                    (uint8_t) THROTTLE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);

            ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t) ui16_wheel_speed_x10,
                    (uint8_t) 40,
                    (uint8_t) 255,
                    (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                    (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        }

        // set battery current target
        ui8_adc_battery_current_target = ui8_adc_battery_current_target_throttle;

        // set duty cycle target
        ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
    }
}

static void apply_pwm_calibration_assist() {

    // disable FOC angle advance
    m_configuration_variables.ui8_foc_angle_multiplicator = 0;

    // ui8_riding_mode_parameter contains the target duty cycle
    uint8_t ui8_calibration_assist_duty_cycle_target = ui8_riding_mode_parameter;

    // limit cadence assist duty cycle target
    if (ui8_calibration_assist_duty_cycle_target >= PWM_DUTY_CYCLE_MAX) {
        ui8_calibration_assist_duty_cycle_target = (uint8_t)(PWM_DUTY_CYCLE_MAX-1);
    }

    // set motor acceleration
    ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
    ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;

    // set battery current target
    ui8_adc_battery_current_target = ui8_adc_battery_current_max;

    // set duty cycle target
    ui8_duty_cycle_target = ui8_calibration_assist_duty_cycle_target;
}

static void apply_erps_calibration_assist() {

    // disable FOC angle advance
    m_configuration_variables.ui8_foc_angle_multiplicator = 0;

    static uint8_t ui8_calibration_assist_duty_cycle_target;

    // ui8_riding_mode_parameter contains the target erpm
    uint8_t ui8_calibration_assist_erpm_target = ui8_riding_mode_parameter;

    if (ui16_motor_speed_erps < ui8_calibration_assist_erpm_target) {
        ui8_calibration_assist_duty_cycle_target++;
    } else if (ui16_motor_speed_erps > ui8_calibration_assist_erpm_target) {
        ui8_calibration_assist_duty_cycle_target--;
    }

    // limit cadence assist duty cycle target
    if (ui8_calibration_assist_duty_cycle_target >= PWM_DUTY_CYCLE_MAX) {
        ui8_calibration_assist_duty_cycle_target = (uint8_t)(PWM_DUTY_CYCLE_MAX-1);
    }

    // set motor acceleration
    ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
    ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;

    // set battery current target
    ui8_adc_battery_current_target = ui8_adc_battery_current_max;

    // set duty cycle target
    ui8_duty_cycle_target = ui8_calibration_assist_duty_cycle_target;
}


static void apply_temperature_limiting() 
{
    // get ADC measurement
    volatile uint16_t ui16_temp = ui16_adc_throttle;

    // filter ADC measurement to motor temperature variable
    ui16_adc_motor_temperature_filtered = filter(ui16_temp, ui16_adc_motor_temperature_filtered, 8);

    // check if above 124degC (ADC value 255)
    if(ui16_adc_motor_temperature_filtered > 255) {
        ui8_adc_battery_current_target = 0;
    } else {
        // convert ADC value
        ui8_motor_temperature_filtered = (uint8_t)(ui16_adc_motor_temperature_filtered * 39 / 80);
        // min temperature value can not be equal or higher than max temperature value
        if (ui8_motor_temperature_min_value_to_limit >= ui8_motor_temperature_max_value_to_limit) {
            ui8_adc_battery_current_target = 0;
        } else {
            // adjust target current if motor over temperature limit
            ui8_adc_battery_current_target = map_ui8(ui8_motor_temperature_filtered,
                ui8_motor_temperature_min_value_to_limit,
                ui8_motor_temperature_max_value_to_limit,
                ui8_adc_battery_current_target,
                0);
        }
    }
}

static void apply_speed_limit() 
{
    if (m_configuration_variables.ui8_wheel_speed_max > 0) {
        // set battery current target limit based on speed limit, faster versions works up to limit of 90km/h
        if (m_configuration_variables.ui8_wheel_speed_max > 50) { // shift down to avoid use of slow map_ui16 function
            ui8_adc_battery_current_target = map_ui8((uint8_t)((ui16_wheel_speed_x10 >> 1) - 200),
                (uint8_t)(((uint16_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)210U),
                (uint8_t)(((uint16_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)195U),
                ui8_adc_battery_current_target,
                0);
        } else {
            ui8_adc_battery_current_target = map_ui8((uint8_t)(ui16_wheel_speed_x10 >> 1),
                (uint8_t)(((uint8_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)10U), // ramp down from 2km/h under
                (uint8_t)(((uint8_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) + (uint8_t)5U), // to 1km/h over
                ui8_adc_battery_current_target,
                0);
        } 
    }
}

static void calc_wheel_speed(void) 
{
    // calc wheel speed (km/h*10)
    if (ui16_wheel_speed_sensor_ticks) {
        uint16_t ui16_tmp = ui16_wheel_speed_sensor_ticks >> 5;
        // rps = PWM_CYCLES_SECOND / ui16_wheel_speed_sensor_ticks (rev/sec)
        // km/h*10 = rps * ui16_wheel_perimeter * ((3600 / (1000 * 1000)) * 10)
        // !!!warning if PWM_CYCLES_SECOND is not a multiple of 1000
        ui16_wheel_speed_x10 =  ui16_wheel_calc_const / ui16_tmp;
    } else {
        ui16_wheel_speed_x10 = 0;
    }
}

static void calc_cadence(void) 
{
    // get the cadence sensor ticks
    uint16_t ui16_cadence_sensor_ticks_temp = ui16_cadence_sensor_ticks;

    // adjust cadence sensor ticks counter min depending on wheel speed
    uint8_t ui8_temp = map_ui8((uint8_t)(ui16_wheel_speed_x10 >> 2),
            10 /* 40 >> 2 */,
            100 /* 400 >> 2 */,
            (CADENCE_SENSOR_CALC_COUNTER_MIN >> 4),
            (CADENCE_SENSOR_TICKS_COUNTER_MIN_AT_SPEED >> 4));

    ui16_cadence_ticks_count_min_speed_adj = (uint16_t)ui8_temp << 4;

    // calculate cadence in RPM and avoid zero division
    // !!!warning if PWM_CYCLES_SECOND > 21845
    if (ui16_cadence_sensor_ticks_temp) {
        ui8_pedal_cadence_RPM = (uint8_t)((PWM_CYCLES_SECOND * 3U) / ui16_cadence_sensor_ticks_temp);
    } else {
        ui8_pedal_cadence_RPM = 0;
    }

    /*-------------------------------------------------------------------------------------------------

     NOTE: regarding the cadence calculation

     Cadence is calculated by counting how many ticks there are between two LOW to HIGH transitions.

     Formula for calculating the cadence in RPM:

     (1) Cadence in RPM = (60 * PWM_CYCLES_SECOND) / CADENCE_SENSOR_NUMBER_MAGNETS) / ticks

     (2) Cadence in RPM = (PWM_CYCLES_SECOND * 3) / ticks

     -------------------------------------------------------------------------------------------------*/
}

#define TOFFSET_START_CYCLES 130 // Torque offset calculation stars after 130 cycles 
#define TOFFSET_END_CYCLES 170   // Torque offset calculation ends after 170 cycles 
static uint8_t toffset_cycle_counter = 0;

static uint8_t ui8_TSamples[81];
static uint8_t ui8_TSamplesNum = 0;
static uint8_t ui8_TSamplesPos = 0;
static uint16_t ui16_TSum = 0;
static uint8_t ui8_TorqueSmoothCnt = 0;
static uint8_t ui8_TorqueMin_tmp = 0;
static uint8_t ui8_TorqueMax_tmp = 0;
static uint8_t ui8_TorqueAVG = 0;
static uint8_t ui8_TorqueMin = 0;
static uint8_t ui8_TorqueMax = 0;


// PWM IRQ set ui8_pas_new_transition when a new PAS signal transition is detected.
// 80 transtions/revolution (one every 4.5 deg)
// @120 rmp: 160 transitions/sec 1 every 6,25 ms
void new_torque_sample() {

    uint16_t ui16_TorqueDeltaADC;
    uint8_t  ui8_TorqueDeltaADC;

    if (ui8_pas_new_transition & 0x80) {
    	// Pedal stop or backward rotation -> reset all
        ui8_pas_new_transition = 0;
        ui8_TSamplesNum = 0;
        ui16_TSum = 0;
        ui8_TSamplesPos = 0;
        ui8_TorqueMin = 0;
        ui8_TorqueMax = 0;
        ui8_TorqueAVG = 0;
        return;
    }

    ui8_pas_new_transition = 0;

    ui16_TorqueDeltaADC = ui16_adc_torque;

    if (ui16_adc_pedal_torque_offset > ui16_TorqueDeltaADC) {
    	// torque adc value less than 0 torque reference ADC -> reset all
        ui8_TSamplesNum = 0;
        ui16_TSum = 0;
        ui8_TSamplesPos = 0;
        ui8_TorqueMin = 0;
        ui8_TorqueMax = 0;
        ui8_TorqueAVG = 0;
        return;
    }

    ui16_TorqueDeltaADC = ui16_TorqueDeltaADC - ui16_adc_pedal_torque_offset;
    if (ui16_TorqueDeltaADC > 255)
    	ui8_TorqueDeltaADC = 255;
    else
    	ui8_TorqueDeltaADC = ui16_TorqueDeltaADC & 0xff;

    ui8_TSamples[ui8_TSamplesPos++] = ui8_TorqueDeltaADC;
    // Add to the average the new sample
    ui16_TSum += ui16_TorqueDeltaADC;
    if (ui8_TorqueDeltaADC > ui8_TorqueMax_tmp)
    	ui8_TorqueMax_tmp = ui8_TorqueDeltaADC;
    if (ui8_TorqueDeltaADC < ui8_TorqueMin_tmp)
    	ui8_TorqueMin_tmp = ui8_TorqueDeltaADC;

    if (ui8_TSamplesPos > 80) {
        ui8_TSamplesPos = 0;
    }
    // Now ui8_TSamples[ui8_TSamplesPos] contains the torque delta ADC of the same pedal position at the previous pedal stroke

    if (ui8_TSamplesNum == 80) {
        // Remove from the average the sample at the same pedal position of the previous pedal stroke
        ui16_TSum -= ui8_TSamples[ui8_TSamplesPos];
        if (ui8_TSamplesPos == 0) {
        	// Save values of the last complete pedal stroke
			ui8_TorqueMin = ui8_TorqueMin_tmp;
			ui8_TorqueMax = ui8_TorqueMax_tmp;
			ui8_TorqueAVG = ui16_TSum/80;
			ui8_TorqueMin_tmp = 255;
			ui8_TorqueMax_tmp = 0;
        }
    } else {
        ui8_TSamplesNum++;
    }
}

static void get_pedal_torque(void) {
	uint16_t ui16_tmp;
    if (toffset_cycle_counter < TOFFSET_END_CYCLES) {
    	if (toffset_cycle_counter > TOFFSET_START_CYCLES) {
			ui16_tmp = ui16_adc_torque;
			ui16_adc_pedal_torque_offset = filter(ui16_tmp, ui16_adc_pedal_torque_offset, 2);
    	}
        toffset_cycle_counter++;
        if (toffset_cycle_counter == TOFFSET_END_CYCLES) {
            ui16_adc_pedal_torque_offset += ADC_TORQUE_SENSOR_CALIBRATION_OFFSET;
            if((ui8_coaster_brake_enabled)&&(ui16_adc_pedal_torque_offset > ui8_coaster_brake_torque_threshold))
				ui16_adc_coaster_brake_threshold = ui16_adc_pedal_torque_offset - ui8_coaster_brake_torque_threshold;
			else
				ui16_adc_coaster_brake_threshold = 0;
        }
        ui16_adc_pedal_torque_delta = 0;
    } else {
        if (ui8_pas_new_transition)
            new_torque_sample();
        
        // get adc pedal torque
        ui16_tmp = ui16_adc_torque;

        if (ui16_tmp > ui16_adc_pedal_torque_offset) {
            ui16_adc_pedal_torque_delta = ui16_tmp - ui16_adc_pedal_torque_offset;
            uint8_t ui8_tmp;
            if (ui16_adc_pedal_torque_delta & (uint16_t)0xff00)
                ui8_tmp = 0xff;
            else
                ui8_tmp = ui16_adc_pedal_torque_delta & 0xff;

            if (m_configuration_variables.ui8_torque_smooth_enabled) {
                if ((ui8_TSamplesNum == 80) && (ui8_tmp > m_configuration_variables.ui8_torque_smooth_min)) {
                    if (ui8_tmp > ui8_TSamples[ui8_TSamplesPos]) {
                        ui8_tmp =  ui8_tmp - ui8_TSamples[ui8_TSamplesPos];
                    } else {
                        ui8_tmp = ui8_TSamples[ui8_TSamplesPos] - ui8_tmp;
                    }
                    if (ui8_tmp < m_configuration_variables.ui8_torque_smooth_max) {
                        ui16_adc_pedal_torque_delta = ui16_TSum / ((uint8_t)80);
                        ui8_TorqueSmoothCnt++;
                    }
                }
            }
        } else {
            ui16_adc_pedal_torque_delta = 0;
        }
    }
}

void get_battery_voltage(void) {

    // read battery voltage and filter
    ui16_adc_battery_voltage_filtered = (ui16_adc_voltage + (ui16_adc_battery_voltage_filtered << 1)) / 3;
    // convert for other uses    
    ui16_battery_voltage_filtered_x1000 = ui16_adc_battery_voltage_filtered * BATTERY_VOLTAGE_PER_10_BIT_ADC_STEP_X1000;

}

void calc_motor_erps(void) {
    
    // calculate motor ERPS
    uint16_t ui16_tmp = ui16_hall_counter_total;
    if (((uint8_t)(ui16_tmp>>8)) & 0x80)
        ui16_motor_speed_erps = 0;
    else
        // Reduce operands to 16 bit (Avoid slow _divulong() library function)
        ui16_motor_speed_erps = (uint16_t)(HALL_COUNTER_FREQ >> 2) / (uint16_t)(ui16_tmp >> 2);

}

static void check_system()
{
    #define CHECK_SPEED_SENSOR_COUNTER_THRESHOLD          250 // 250 * 30ms = 7.5 seconds
    #define MOTOR_ERPS_SPEED_THRESHOLD	                  180
	static uint8_t ui8_check_speed_sensor_counter;
	static uint8_t ui8_error_speed_sensor_counter;

	// check speed sensor
	if(ui16_motor_speed_erps > MOTOR_ERPS_SPEED_THRESHOLD) { ui8_check_speed_sensor_counter++; }
	if(ui16_wheel_speed_x10) { ui8_check_speed_sensor_counter = 0; }

	if(ui8_check_speed_sensor_counter > CHECK_SPEED_SENSOR_COUNTER_THRESHOLD) {
		// set speed sensor error code
		ui8_m_system_state |= ERROR_SPEED_SENSOR;
	}
	else if (ui8_m_system_state & ERROR_SPEED_SENSOR) {
		// increment speed sensor error reset counter
        ui8_error_speed_sensor_counter++;

        // check if the counter has counted to the set threshold for reset
        if (ui8_error_speed_sensor_counter > CHECK_SPEED_SENSOR_COUNTER_THRESHOLD) {
            // reset speed sensor error code
            if (ui8_m_system_state & ERROR_SPEED_SENSOR) {										   
				ui8_m_system_state &= ~ERROR_SPEED_SENSOR;
				ui8_error_speed_sensor_counter = 0;
            }
		}
	}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    #define CHECK_CADENCE_SENSOR_COUNTER_THRESHOLD          167 // 200 * 30ms = 5 seconds
    #define CADENCE_SENSOR_RESET_COUNTER_THRESHOLD         	100 // 100 * 30ms = 3 seconds
    #define ADC_TORQUE_SENSOR_DELTA_THRESHOLD				80 // to check		

	static uint16_t ui8_check_cadence_sensor_counter;
	static uint8_t ui8_error_cadence_sensor_counter;

	// check cadence sensor
	if((ui16_adc_pedal_torque_delta > ADC_TORQUE_SENSOR_DELTA_THRESHOLD)&&
	  ((ui8_pedal_cadence_RPM > 130)||(!ui8_pedal_cadence_RPM))) {
		ui8_check_cadence_sensor_counter++;
	}
	else {
		ui8_check_cadence_sensor_counter = 0;
	}

	if(ui8_check_cadence_sensor_counter > CHECK_CADENCE_SENSOR_COUNTER_THRESHOLD) {
		// set cadence sensor error code
		ui8_m_system_state |= ERROR_CADENCE_SENSOR;
	}
	else if (ui8_m_system_state & ERROR_CADENCE_SENSOR) {
		// increment cadence sensor error reset counter
        ui8_error_cadence_sensor_counter++;

        // check if the counter has counted to the set threshold for reset
        if (ui8_error_cadence_sensor_counter > CADENCE_SENSOR_RESET_COUNTER_THRESHOLD) {
            // reset cadence sensor error code
            if (ui8_m_system_state & ERROR_CADENCE_SENSOR) {
				ui8_m_system_state &= ~ERROR_CADENCE_SENSOR;
				ui8_error_cadence_sensor_counter = 0;
            }
		}
	}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // check torque sensor
    if (((ui16_adc_pedal_torque_offset > 300) || (ui16_adc_pedal_torque_offset < 10) || (ui16_adc_torque > 500))
            && ((ui8_riding_mode == POWER_ASSIST_MODE) || (ui8_riding_mode == TORQUE_ASSIST_MODE)
                    || (ui8_riding_mode == eMTB_ASSIST_MODE))) {
        // set error code
        ui8_m_system_state = ERROR_TORQUE_SENSOR;
    } else if (ui8_m_system_state & ERROR_TORQUE_SENSOR) {
        // reset error code
        ui8_m_system_state &= ~ERROR_TORQUE_SENSOR;
    }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    #define MOTOR_BLOCKED_COUNTER_THRESHOLD               	    10 // 10 * 30ms => 0.3 second
    #define MOTOR_BLOCKED_BATTERY_CURRENT_THRESHOLD_X10   	    50 // 30 = 3.0 amps
    #define MOTOR_BLOCKED_BATTERY_CURRENT_THRESHOLD_ADC_STEPS   31 // 19 * 0.16A/step = 3.04 amps
    #define MOTOR_BLOCKED_ERPS_THRESHOLD                  	    10 // 20 ERPS
    #define MOTOR_BLOCKED_RESET_COUNTER_THRESHOLD         	    250 // 250 * 30ms = 7.5 seconds

    static uint8_t ui8_motor_blocked_counter;
    static uint8_t ui8_motor_blocked_reset_counter;

    // Check battery Over-current (read current here in case PWM interrupt for some error was disabled)
    // Read in assembler to ensure data consistency (conversion overrun)
    #ifndef __CDT_PARSER__ // avoid Eclipse syntax check
    __asm
        ldw x, 0x53ea // ADC1->DB5RH
        cpw x, 0x53ea // ADC1->DB5RH
        jreq 00010$
        ldw x, 0x53ea // ADC1->DB5RH
    00010$:
        cpw x, #ADC_10_BIT_BATTERY_OVERCURRENT
        jrc 00011$
        mov _ui8_m_system_state+0, #ERROR_BATTERY_OVERCURRENT
    00011$:
    __endasm;
    #endif

    if (ui8_m_system_state == ERROR_BATTERY_OVERCURRENT)
        return;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // if the motor blocked error is enabled start resetting it
    if (ui8_m_system_state & ERROR_MOTOR_BLOCKED) {
        // increment motor blocked reset counter
        ui8_motor_blocked_reset_counter++;

        // check if the counter has counted to the set threshold for reset
        if (ui8_motor_blocked_reset_counter > MOTOR_BLOCKED_RESET_COUNTER_THRESHOLD) {
            // reset motor blocked error code
             if (ui8_m_system_state & ERROR_MOTOR_BLOCKED) {
                ui8_m_system_state &= ~ERROR_MOTOR_BLOCKED;
            }

            // reset the counter that clears the motor blocked error
            ui8_motor_blocked_reset_counter = 0;
        }
    } else {
        // if battery current is over the current threshold and the motor ERPS is below threshold start setting motor blocked error code
        if ((ui8_adc_battery_current_filtered > MOTOR_BLOCKED_BATTERY_CURRENT_THRESHOLD_ADC_STEPS)
                && (ui16_motor_speed_erps < MOTOR_BLOCKED_ERPS_THRESHOLD)) {
            // increment motor blocked counter with 100 milliseconds
            ++ui8_motor_blocked_counter;

            // check if motor is blocked for more than some safe threshold
            if (ui8_motor_blocked_counter > MOTOR_BLOCKED_COUNTER_THRESHOLD) {
                // set error code
            ui8_m_system_state |= ERROR_MOTOR_BLOCKED;

                // reset motor blocked counter as the error code is set
                ui8_motor_blocked_counter = 0;
            }
        } else {
            // current is below the threshold and/or motor ERPS is above the threshold so reset the counter
            ui8_motor_blocked_counter = 0;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void ebike_control_lights(void)
{
    // select lights configuration
    switch (ui8_lights_configuration) {
    case 0:

        // set lights
        lights_set_state(ui8_lights_state);

        break;

    case 5:

        // check brake state
        if (ui8_brake_state) {
            // set lights
            lights_set_state(ui8_brake_state);
        } else {
            // set lights
            lights_set_state(ui8_lights_state);
        }

        break;

    default:

        // set lights
        lights_set_state(ui8_lights_state);

        break;
    }

    /*------------------------------------------------------------------------------------------------------------------
     NOTE: regarding the various light modes
     (0) lights ON when enabled
     *** (1) lights FLASHING when enabled
     *** (2) lights ON when enabled and BRAKE-FLASHING when braking
     *** (3) lights FLASHING when enabled and ON when braking
     *** (4) lights FLASHING when enabled and BRAKE-FLASHING when braking
     (5) lights ON when enabled, but ON when braking regardless if lights are enabled
     *** (6) lights ON when enabled, but BRAKE-FLASHING when braking regardless if lights are enabled
     *** (7) lights FLASHING when enabled, but ON when braking regardless if lights are enabled
     *** (8) lights FLASHING when enabled, but BRAKE-FLASHING when braking regardless if lights are enabled
     ------------------------------------------------------------------------------------------------------------------*/
}

#ifdef __CDT_PARSER__
#define __interrupt(x)
#endif

// from v.1.1.0 ********************************************************************************************************
// This is the interrupt that happens when UART2 receives data.

void UART2_RX_IRQHandler(void) __interrupt(UART2_RX_IRQHANDLER)
{
    if (UART2->SR & 0x20) {
        //Write the recieved data to the ringbuffer at the write index position, move write index forward.
        ui8_rx_ringbuffer[(uint8_t)(ui8_rx_ringbuffer_write_index++)] = ((uint8_t)UART2->DR);//UART2_ReceiveData8(); save a few cycles...
        // If write index hits the read index - move read index forward. Effectively overwrites the oldest data in the buffer.
        if (((uint8_t)ui8_rx_ringbuffer_write_index)==(uint8_t)(ui8_rx_ringbuffer_read_index)) ui8_rx_ringbuffer_read_index++;
    }
}


void UART2_TX_IRQHandler(void) __interrupt(UART2_TX_IRQHANDLER)
{
	if (UART2->SR & 0x80) // save a few cycles
	{
		if (ui8_m_tx_buffer_index < ui8_packet_len)  // bytes to send
		{
			// clearing the TXE bit is always performed by a write to the data register
			UART2->DR = ui8_tx_buffer[ui8_m_tx_buffer_index];
			++ui8_m_tx_buffer_index;
			if (ui8_m_tx_buffer_index == ui8_packet_len)
			{
				// buffer empty
				// UART2_ITConfig(UART2_IT_TXE, DISABLE);
                UART2->CR2 &= 0x7F; // disable TIEN (TXE)
			}
		}
	}
	else
	{
		// TXE interrupt should never occur if there is nothing to send in the buffer
		// send a zero to clear TXE and disable the interrupt
		UART2->DR = 0;
		// UART2_ITConfig(UART2_IT_TXE, DISABLE);
        UART2->CR2 &= 0x7F;
	}
}

// Read the input buffer and assemble data as a package and signal that we have a package to process (on main slow loop)
static void packet_assembler(void)
{
  if (((uint8_t)ui8_rx_ringbuffer_read_index)!=((uint8_t)ui8_rx_ringbuffer_write_index))
  {
    if (ui8_received_package_flag == 0) // only when package were previously processed
    {
      while (((uint8_t)(ui8_rx_ringbuffer_read_index) != ((uint8_t)ui8_rx_ringbuffer_write_index)))
      {
        ui8_byte_received = ui8_rx_ringbuffer[(uint8_t)(ui8_rx_ringbuffer_read_index++)];
        
        switch (ui8_state_machine)
        {
          case 0:
          if (ui8_byte_received == 0x59) { // see if we get start package byte
            ui8_rx_buffer[0] = ui8_byte_received;
            ui8_state_machine = 1;
          }
          else
            ui8_state_machine = 0;
          break;

          case 1:
            ui8_rx_buffer[1] = ui8_byte_received;
            ui8_rx_len = ui8_byte_received;
            ui8_state_machine = 2;
          break;

          case 2:
          ui8_rx_buffer[ui8_rx_cnt + 2] = ui8_byte_received;
          ++ui8_rx_cnt;

          if (ui8_rx_cnt >= ui8_rx_len)
          {
            ui8_rx_cnt = 0;
            ui8_state_machine = 0;
            ui8_received_package_flag = 1; // signal that we have a full package to be processed
            return;
          }
          break;

          default:
          break;
        }
      }
    }
    else // if there was any error, restart our state machine
    {
      ui8_rx_cnt = 0;
      ui8_state_machine = 0;
    }
  }
}

void communications_controller(void)
{
  uint8_t ui8_frame_type_to_send = 0;
  uint8_t ui8_len;

  if (ui8_received_package_flag) {
    // just to make easy next calculations
    ui16_crc_rx = 0xffff;
    ui8_len = ui8_rx_buffer[1];
    for (ui8_i = 0; ui8_i < ui8_len; ui8_i++) {
      crc16(ui8_rx_buffer[ui8_i], &ui16_crc_rx);
    }
    // if CRC is correct read the package
    if (((((uint16_t) ui8_rx_buffer[ui8_len + 1]) << 8) + ((uint16_t) ui8_rx_buffer[ui8_len])) == ui16_crc_rx) {
        ui8_comm_error_counter = 0;
        if (ui8_m_motor_init_state == MOTOR_INIT_STATE_RESET) {
            ui8_m_motor_init_state = MOTOR_INIT_STATE_NO_INIT;
        }
        ui8_frame_type_to_send = ui8_rx_buffer[2];
        communications_process_packages(ui8_frame_type_to_send);
    } else {
        ui8_received_package_flag = 0;
        ui8_comm_error_counter++;
    }
  } else {
      ui8_comm_error_counter++;
  }

  // check for communications fail or display master fail
  // can't fail more then 900ms (20 x 60ms loop)
  if (ui8_comm_error_counter > 15) {
    motor_disable_pwm();
    ui8_motor_enabled = 0;
    ui8_m_system_state |= ERROR_FATAL;
  }

  if (ui8_m_motor_init_state == MOTOR_INIT_STATE_RESET) {
    communications_process_packages(COMM_FRAME_TYPE_ALIVE);
  }
}

static void communications_process_packages(uint8_t ui8_frame_type)
{
	uint8_t ui8_temp;
	uint16_t ui16_temp;
	uint8_t ui8_len = 3; // 3 bytes: 1 type of frame + 2 CRC bytes

	// start up byte
	ui8_tx_buffer[0] = 0x43;
	ui8_tx_buffer[2] = ui8_frame_type;

	// prepare payload
	switch (ui8_frame_type)
	{
	  // periodic data
	  case COMM_FRAME_TYPE_PERIODIC:
		// display will send periodic command after motor init ok, now reset so the state machine will be ready for next time
		ui8_m_motor_init_status = MOTOR_INIT_STATUS_RESET;

        // riding mode
        // see section below in Cruise and Walk Assist section - ui8_riding_mode = ui8_rx_buffer[3];

		// riding mode parameter
		ui8_riding_mode_parameter = ui8_rx_buffer[4];

		// hybrid torque parameter
		ui8_hybrid_torque_parameter = ui8_rx_buffer[5];

    	// walk assist parameter
		ui8_walk_assist_parameter = ui8_rx_buffer[6];

        // battery max power target
		m_configuration_variables.ui8_target_battery_max_power_div25 = ui8_rx_buffer[7];

		// calculate max battery current in ADC steps from the received battery current limit
		// uint8_t ui8_adc_battery_current_max_temp_1 = (uint16_t)(ui8_battery_current_max * 100) / (uint16_t)BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;
        // speed up as BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100 = 16
        uint8_t ui8_adc_battery_current_max_temp_1 = (uint16_t)(ui8_battery_current_max * 25) >> 2;
    
		// calculate max battery current in ADC steps from the received power limit
        /* uint32_t ui32_battery_current_max_x100 = ((uint32_t) m_configuration_variables.ui8_target_battery_max_power_div25 * 2500000)
					/ ui16_battery_voltage_filtered_x1000;
		uint8_t ui8_adc_battery_current_max_temp_2 = ui32_battery_current_max_x100 / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100; */
        // see main.h for calculation
		uint8_t ui8_adc_battery_current_max_temp_2 = (m_configuration_variables.ui8_target_battery_max_power_div25 * MAX_POWER_CALC_CONST_DIV8) / (ui16_adc_battery_voltage_filtered >> 3);

		// set max battery current
		ui8_adc_battery_current_max = ui8_min(ui8_adc_battery_current_max_temp_1, ui8_adc_battery_current_max_temp_2);

    	// wheel max speed
		m_configuration_variables.ui8_wheel_speed_max = ui8_rx_buffer[8];
	
    	// lights state
		ui8_lights_state = ((ui8_rx_buffer[9] >> 0) & 1);

		// walk assist
		ui8_walk_assist = ((ui8_rx_buffer[9] >> 1) & 1);

        // cruise enabled
		ui8_cruise_enabled = ((ui8_rx_buffer[9] >> 2) & 1);

		// adjust riding mode
        if ((ui8_walk_assist)&&(ui16_wheel_speed_x10 < WALK_ASSIST_THRESHOLD_SPEED_X10)) {
			// enable walk assist depending on speed
			ui8_riding_mode = WALK_ASSIST_MODE;
        } else if ((ui8_cruise_enabled)&&(ui16_wheel_speed_x10 > CRUISE_THRESHOLD_SPEED_X10)) {
			// enable cruise function depending on speed
			ui8_riding_mode = CRUISE_MODE;
        } else {
            ui8_riding_mode = ui8_rx_buffer[3];
        }

        // motor temperature limit function or throttle
        // optional ADC function, temperature sensor or throttle or not in use
		m_configuration_variables.ui8_optional_ADC_function = ((ui8_rx_buffer[9] >> 3) & 3);

		// virtual throttle
		ui8_throttle_virtual = ui8_rx_buffer[10];

		// Now send data back 

		// send battery_current_x5
        // ui8_battery_current_filtered_x10 = (uint8_t)(((uint16_t)ui8_adc_battery_current_filtered * BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100) / 10);
        // BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100 = 16
		ui8_tx_buffer[3] = (uint8_t)(((uint16_t)ui8_adc_battery_current_filtered << 2) / 5); // ui8_battery_current_filtered_x10 / 2;

		// ADC 10 bits battery voltage
	    ui8_tx_buffer[4] = (ui16_adc_battery_voltage_filtered & 0xff);

		// wheel speed
		ui8_tx_buffer[5] = (uint8_t) (ui16_wheel_speed_x10 & 0xff);
        
        // high bits of wheel speed and battery voltage
		ui8_tx_buffer[6] = (uint8_t) (((ui16_wheel_speed_x10 >> 8) & 0x0f) | ((ui16_adc_battery_voltage_filtered >> 4) & 0xf0));

        // brake state
	    ui8_tx_buffer[7] = ui8_brake_state;

        // adjusted throttle value or temperature limit depending on user setup
		if(m_configuration_variables.ui8_optional_ADC_function == TEMPERATURE_CONTROL)
		{
			// temperature value
			ui8_tx_buffer[8] = ui8_motor_temperature_filtered;
		}
		else
		{
			// throttle value with offset removed and mapped to 255
			ui8_tx_buffer[8] = ui8_throttle_adc;
		}

		// throttle value from ADC
		ui8_tx_buffer[9] = (uint8_t)(ui16_adc_throttle >> 2);

		// ADC torque_sensor
		ui8_tx_buffer[10] = (uint8_t) (ui16_adc_torque & 0xff);
		
        // pedal torque delta
		ui8_tx_buffer[11] = (uint8_t) (ui16_adc_pedal_torque_delta & 0xff);

		// pedal torque delta boost
		ui8_tx_buffer[12] = (uint8_t) (ui16_adc_pedal_torque_power_mode & 0xff);

        // high bits of torque sensor variables
        ui8_tx_buffer[13] = (uint8_t)((ui16_adc_torque >> 8) & 0x03) | 
                                (uint8_t)((ui16_adc_pedal_torque_delta >> 6) & 0x0C) |
                                (uint8_t)((ui16_adc_pedal_torque_power_mode >> 4) & 0xF0);

		// PAS cadence
		ui8_tx_buffer[14] = ui8_pedal_cadence_RPM;

		// PWM duty_cycle 
		ui8_tx_buffer[15] = ui8_g_duty_cycle; // convert to 0-100% in display receive
        
		// motor speed in ERPS
		ui8_tx_buffer[16] = (uint8_t) (ui16_motor_speed_erps & 0xff);
		ui8_tx_buffer[17] = (uint8_t) (ui16_motor_speed_erps >> 8);

		// FOC angle
		ui8_tx_buffer[18] = ui8_g_foc_angle;

		// system state
		ui8_tx_buffer[19] = ui8_m_system_state;

		// wheel_speed_sensor_tick_counter
		ui8_tx_buffer[20] = (uint8_t) (ui32_wheel_speed_sensor_ticks_total & 0xff);
		ui8_tx_buffer[21] = (uint8_t) ((ui32_wheel_speed_sensor_ticks_total >> 8) & 0xff);
		ui8_tx_buffer[22] = (uint8_t) ((ui32_wheel_speed_sensor_ticks_total >> 16) & 0xff);

        // ADC battery current
		ui8_tx_buffer[23] = ui8_adc_motor_phase_current;

        // Hall sensor state
		ui8_tx_buffer[24] = ui8_hall_sensors_state;

		ui8_len += 22;
		break;

	  // set configurations
	  case COMM_FRAME_TYPE_CONFIGURATIONS:
		// disable the motor to avoid a quick of the motor while configurations are changed
		// disable the motor, lets hope this is safe to do here, in this way
		// the motor shold be enabled again on the ebike_control_motor()
		motor_disable_pwm();
		ui8_motor_enabled = 0;
		ui8_m_system_state |= ERROR_NOT_INIT;
		ui8_m_motor_init_state = MOTOR_INIT_STATE_INIT_START_DELAY;
		ui8_m_motor_init_status = MOTOR_INIT_STATUS_GOT_CONFIG;

		// battery low voltage cut-off x10
		m_configuration_variables.ui16_battery_low_voltage_cut_off_x10 = (((uint16_t) ui8_rx_buffer[4]) << 8) + ((uint16_t) ui8_rx_buffer[3]);

		// set low voltage cutoff (16 bit)
		ui16_adc_voltage_cut_off = (m_configuration_variables.ui16_battery_low_voltage_cut_off_x10 * 100U) / BATTERY_VOLTAGE_PER_10_BIT_ADC_STEP_X1000;

		// wheel perimeter
		m_configuration_variables.ui16_wheel_perimeter = (((uint16_t) ui8_rx_buffer[6]) << 8) + ((uint16_t) ui8_rx_buffer[5]);
        ui16_wheel_calc_const = (uint16_t)((((uint32_t)m_configuration_variables.ui16_wheel_perimeter) * PWM_CYCLES_SECOND / 1000 * 36U) >> 5);

		// battery max current
		ui8_battery_current_max = ui8_rx_buffer[7];

		// config bits
		ui8_startup_boost_enabled = ui8_rx_buffer[8] & 1;
		// ui8_torque_sensor_calibration_enabled = (ui8_rx_buffer[8] >> 1) & 1;
		m_configuration_variables.ui8_torque_smooth_enabled = (ui8_rx_buffer[8] >> 1) & 1;
        ui8_assist_whit_error_enabled = (ui8_rx_buffer[8] >> 2) & 1;
		ui8_assist_without_pedal_rotation_enabled = (ui8_rx_buffer[8] >> 3) & 1;
        // motor type here
        ui8_coaster_brake_enabled = (ui8_rx_buffer[8] >> 5) & 1;
        ui8_g_field_weakening_enable = (ui8_rx_buffer[8] >> 6) & 1;
        ui8_brake_fast_stop = (ui8_rx_buffer[8] >> 7) & 1;

        // motor type
        ui8_temp = (ui8_rx_buffer[8] >> 4) & 1;
		//m_configuration_variables.ui8_motor_inductance_x1048576
		// motor inductance & cruise pid parameter
		if(ui8_temp == 0)
		{
			// 48 V motor
			m_configuration_variables.ui8_foc_angle_multiplicator = FOC_MULTIPLICATOR_48V;
			i16_cruise_pid_kp = 12;
			i16_cruise_pid_ki = 1;
		}
		else
		{
			// 36 V motor
			m_configuration_variables.ui8_foc_angle_multiplicator = FOC_MULTIPLICATOR_36V;
			i16_cruise_pid_kp = 14;
			i16_cruise_pid_ki = 0.7;
		}

		// startup boost
		ui8_startup_boost_factor_array[0] = ui8_rx_buffer[9];
		ui8_startup_boost_cadence_step = ui8_rx_buffer[10];

		for (ui8_i = 1; ui8_i < 120; ui8_i++)
		{
			ui8_temp = (ui8_startup_boost_factor_array[ui8_i - 1] * ui8_startup_boost_cadence_step) >> 8;
			ui8_startup_boost_factor_array[ui8_i] = ui8_startup_boost_factor_array[ui8_i - 1] - ui8_temp;	
		}

		// motor over temperature min value limit
		ui8_motor_temperature_min_value_to_limit = ui8_rx_buffer[11];
		// motor over temperature max value limit
		ui8_motor_temperature_max_value_to_limit = ui8_rx_buffer[12];

		// motor acceleration adjustment
		uint8_t ui8_motor_acceleration_adjustment = ui8_rx_buffer[13];

		// set duty cycle ramp up inverse step
		ui8_duty_cycle_ramp_up_inverse_step_default = map_ui8((uint8_t)ui8_motor_acceleration_adjustment,
				(uint8_t) 0,
                (uint8_t) 100,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);

        // motor deceleration adjustment
        uint8_t ui8_motor_deceleration_adjustment = ui8_rx_buffer[14];

        // set duty cycle ramp down inverse step
		ui8_duty_cycle_ramp_down_inverse_step_default = map_ui8((uint8_t)ui8_motor_deceleration_adjustment,
				(uint8_t) 0,
                (uint8_t) 100,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);

        // minimum value for torque smoothing
        m_configuration_variables.ui8_torque_smooth_min = ui8_rx_buffer[15];
        // maximum value for torque smoothing
        m_configuration_variables.ui8_torque_smooth_max = ui8_rx_buffer[16];

		// coast brake threshold
		ui8_coaster_brake_torque_threshold = ui8_rx_buffer[17];

		// lights configuration
		ui8_lights_configuration = ui8_rx_buffer[18];

		// torque sensor adc step (default 67)
		m_configuration_variables.ui8_pedal_torque_per_10_bit_ADC_step_x100 = ui8_rx_buffer[19];

		// torque sensor ADC threshold assist without rotation
        if(ui8_rx_buffer[22] > 100) {
            ui8_assist_without_pedal_rotation_threshold = 10;
        } else {
		    ui8_assist_without_pedal_rotation_threshold = 110 - ui8_rx_buffer[20];
        }

        // motor acceleration after braking
        uint8_t ui8_motor_acceleration_adjustment_after_brake = ui8_rx_buffer[21];

        // set duty cycle ramp up inverse step for after braking
		ui8_duty_cycle_ramp_up_inverse_step_after_braking = map_ui8((uint8_t)ui8_motor_acceleration_adjustment_after_brake,
				(uint8_t) 0,
                (uint8_t) 100,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT,
                (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);

        // motor acceleration after braking
        ui8_motor_acceleration_delay_after_brake = ui8_rx_buffer[22];

        // Hall Ref Angles and counter offsets

        if (ui8_rx_buffer[23]) { // check if calibration enabled
            ui8_hall_ref_angles_config[0] = ui8_rx_buffer[24];
            ui8_hall_ref_angles_config[1] = ui8_rx_buffer[25];
            ui8_hall_ref_angles_config[2] = ui8_rx_buffer[26];
            ui8_hall_ref_angles_config[3] = ui8_rx_buffer[27];
            ui8_hall_ref_angles_config[4] = ui8_rx_buffer[28];
            ui8_hall_ref_angles_config[5] = ui8_rx_buffer[29];
            ui8_hall_counter_offsets[0] = ui8_rx_buffer[30];
            ui8_hall_counter_offsets[1] = ui8_rx_buffer[31];
            ui8_hall_counter_offsets[2] = ui8_rx_buffer[32];
            ui8_hall_counter_offsets[3] = ui8_rx_buffer[33];
            ui8_hall_counter_offsets[4] = ui8_rx_buffer[34];
            ui8_hall_counter_offsets[5] = ui8_rx_buffer[35];
        } else {
            ui8_hall_ref_angles_config[0] = PHASE_ROTOR_ANGLE_30;
			ui8_hall_ref_angles_config[1] = PHASE_ROTOR_ANGLE_90;
			ui8_hall_ref_angles_config[2] = PHASE_ROTOR_ANGLE_150;
			ui8_hall_ref_angles_config[3] = PHASE_ROTOR_ANGLE_210;
			ui8_hall_ref_angles_config[4] = PHASE_ROTOR_ANGLE_270;
			ui8_hall_ref_angles_config[5] = PHASE_ROTOR_ANGLE_330;
            ui8_hall_counter_offsets[0] = HALL_COUNTER_OFFSET_UP;
		    ui8_hall_counter_offsets[1] = HALL_COUNTER_OFFSET_DOWN;
            ui8_hall_counter_offsets[2] = HALL_COUNTER_OFFSET_UP;
            ui8_hall_counter_offsets[3] = HALL_COUNTER_OFFSET_DOWN;
            ui8_hall_counter_offsets[4] = HALL_COUNTER_OFFSET_UP;
            ui8_hall_counter_offsets[5] = HALL_COUNTER_OFFSET_DOWN;
        }
        for (ui8_temp = 0; ui8_temp < 6; ui8_temp++) {
            ui8_hall_ref_angles[ui8_temp] = ui8_hall_ref_angles_config[ui8_temp];
        }
        ui8_configurations_changed = 1;

        // reset ringbuffer count - start with new packets
        ui8_rx_ringbuffer_read_index = ui8_rx_ringbuffer_write_index;
	
		break;

      // firmware version
      case COMM_FRAME_TYPE_FIRMWARE_VERSION:
		ui8_tx_buffer[3] = ui8_m_system_state;
		ui8_tx_buffer[4] = 25;
		ui8_tx_buffer[5] = 1;
		ui8_tx_buffer[6] = 1;
		ui8_len += 4;
		break;

      case COMM_FRAME_TYPE_ALIVE:
		// nothing to add
		break;

      case COMM_FRAME_TYPE_STATUS:
		ui8_tx_buffer[3] = ui8_m_motor_init_status;
		ui8_len += 1;
		break;

      case COMM_FRAME_TYPE_HALL_CALBRATION:
        if (ui8_rx_buffer[3] == 1) {
            ui8_riding_mode_parameter = ui8_rx_buffer[4];
            ui8_riding_mode = PWM_CALIBRATION_ASSIST_MODE;
        } else if (ui8_rx_buffer[3] == 2) {
            ui8_riding_mode_parameter = ui8_rx_buffer[4];
            ui8_riding_mode = ERPS_CALIBRATION_ASSIST_MODE;
        } else {
            ui8_riding_mode = OFF_MODE;
        }
        uint8_t ui8_hall_angle_test_offset = ui8_rx_buffer[5];
        for (ui8_temp = 0; ui8_temp < 6; ui8_temp++) {
            ui8_hall_ref_angles[ui8_temp] = ui8_hall_ref_angles_config[ui8_temp] + ui8_hall_angle_test_offset;
        }
        
        // send data back
        ui16_temp = ui16_hall_calib_cnt[0];
        ui8_tx_buffer[3] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[4] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[1];
        ui8_tx_buffer[5] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[6] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[2];
        ui8_tx_buffer[7] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[8] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[3];
        ui8_tx_buffer[9] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[10] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[4];
        ui8_tx_buffer[11] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[12] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[5];
        ui8_tx_buffer[13] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[14] = (uint8_t) (ui16_temp >> 8);

        ui8_tx_buffer[15] = (uint8_t) (ui16_motor_speed_erps & 0xff);
		ui8_tx_buffer[16] = (uint8_t) (ui16_motor_speed_erps >> 8);

        ui8_len += 14;
        break;

      default:
		break;
	}

	ui8_tx_buffer[1] = ui8_len;

	// prepare crc of the package
	ui16_crc_tx = 0xffff;
	for (ui8_i = 0; ui8_i < ui8_len; ui8_i++)
	{
		crc16(ui8_tx_buffer[ui8_i], &ui16_crc_tx);
	}
	ui8_tx_buffer[ui8_len] = (uint8_t) (ui16_crc_tx & 0xff);
	ui8_tx_buffer[ui8_len + 1] = (uint8_t) (ui16_crc_tx >> 8) & 0xff;

	ui8_m_tx_buffer_index = 0;
	// start transmition
    ui8_packet_len = ui8_len + 2;
	UART2->CR2 |= (1 << 7);

	// get ready to get next package
	ui8_received_package_flag = 0;

}
