#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    UI_HOME, UI_MENU, UI_ALARM, UI_TIMER, UI_SET_TIME,
    UI_WIFI, UI_GIFT, UI_LOOK, UI_KEYBOARD
} ui_page_t;

typedef struct {
    ui_page_t page;
    struct tm local;
    bool time_valid, rtc_present, wifi_connected, portal_enabled;
    bool alarm_enabled, alarm_ringing, timer_ringing, timer_running;
    uint8_t alarm_hour, alarm_minute, brightness, theme;
    int timer_minutes, timer_seconds_left;
    char ssid[33], password[65], gift[101], contact[101], portal_password[16], portal_ssid[33];
    bool editing_password, keyboard_lower;
    char keyboard_value[65];
    uint8_t hour, minute, second, day, month;
    int year;
} ui_state_t;

esp_err_t screen_init(void);
void screen_draw(const ui_state_t *state);
bool screen_poll_touch(int *x, int *y);
void screen_set_brightness(uint8_t value);
