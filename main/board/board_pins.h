#pragma once

#include "sdkconfig.h"

/*
 * JC1060P470C — ESP32-P4 pin assignments
 *
 * Backlight GPIO is configurable via Kconfig (GROCY_BACKLIGHT_GPIO).
 * MIPI-DSI pins are handled by the MIPI-DSI peripheral, no GPIO config needed.
 */

/* ── Backlight ── */
#define BOARD_PIN_BACKLIGHT    CONFIG_GROCY_BACKLIGHT_GPIO   /* LEDC PWM output */
#define BOARD_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BOARD_LEDC_TIMER       LEDC_TIMER_0
#define BOARD_LEDC_FREQ_HZ     1000
#define BOARD_LEDC_RESOLUTION  LEDC_TIMER_8_BIT   /* 0–255 duty */
#define BOARD_LEDC_MAX_DUTY    255

/* ── LCD reset ── */
#define BOARD_PIN_LCD_RST      GPIO_NUM_27

/* ── GT911 touch controller (I2C) ── */
#define BOARD_I2C_PORT         I2C_NUM_0
#define BOARD_PIN_I2C_SDA      GPIO_NUM_7
#define BOARD_PIN_I2C_SCL      GPIO_NUM_8
#define BOARD_PIN_TOUCH_RST    GPIO_NUM_22
#define BOARD_PIN_TOUCH_INT    GPIO_NUM_21
#define BOARD_I2C_FREQ_HZ      400000

/* ── Display ── */
#define BOARD_DISPLAY_WIDTH    1024
#define BOARD_DISPLAY_HEIGHT   600
#define BOARD_DISPLAY_HPORCH   (BOARD_DISPLAY_WIDTH  + 60)
#define BOARD_DISPLAY_VPORCH   (BOARD_DISPLAY_HEIGHT + 16)

/* ── Camera (optional) ── */
#define BOARD_PIN_CAM_PWDN     GPIO_NUM_2
