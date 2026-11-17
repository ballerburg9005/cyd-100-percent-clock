#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    UI_HOME, UI_MENU, UI_ALARM, UI_TIMER, UI_SET_TIME,
    UI_WIFI, UI_WIFI_SCAN, UI_CONTACT, UI_LOOK, UI_KEYBOARD, UI_FILES, UI_FILE_VIEW, UI_SOUND_PICK
} ui_page_t;

typedef struct {
    ui_page_t page;
    struct tm local;
    bool time_valid, rtc_present, wifi_connected, portal_enabled, ntp_synced;
    bool wifi_connecting, wifi_scanning;
    uint16_t wifi_disconnect_reason;
    bool alarm_enabled, alarm_ringing, timer_ringing, timer_running;
    bool alert_flash; /* short visual pulse while alarm or timer rings */
    uint8_t brightness, theme;
    uint16_t portal_seconds_left; /* ten-minute setup AP countdown */
    uint8_t wifi_scan_count, wifi_scan_page;
    char wifi_scan_ssids[16][33];
    uint16_t alarm_bp; /* 0..9999 = percent hundredths of a local day */
    uint16_t timer_bp; /* 1..999 = hundredths of a day, up to 9.99% */
    uint32_t timer_duration_ms; /* exact 300000ms default, not rounded 0.35% */
    bool use_ntp;
    int timer_seconds_left;
    char ssid[33], password[65], email[65], xmpp[65], sms[65], portal_password[16], portal_ssid[33];
    char file_name[80], file_preview[240]; bool sd_ready; int file_index;
    char alarm_sound[80],timer_sound[80];  /* NVS: builtin:n or sd:filename */
    uint8_t contact_tune,sound_target; /* 0=not picking, 1=alarm, 2=timer */
    bool audio_playing;
    bool editing_password, keyboard_lower, keyboard_error;
    bool touch_feedback;
    int16_t touch_x, touch_y;
    char keyboard_value[65];
    uint8_t hour, minute, second, day, month;
    int year;
} ui_state_t;

esp_err_t screen_init(void);
void screen_draw(const ui_state_t *state);
bool screen_poll_touch(int *x, int *y, ui_page_t page);
void screen_set_brightness(uint8_t value);
