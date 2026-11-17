#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Call screen_suspend_for_composite() before start and screen_resume_from_composite() after stop. */
bool tv_video_start(const char *filename,bool bmp);
void tv_video_tick(int64_t now_us);
void tv_video_stop(void);
bool tv_video_active(void);
