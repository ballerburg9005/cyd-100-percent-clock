#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MEDIA_BMP_WIDTH 128
#define MEDIA_BMP_HEIGHT 165 /* closest integer to 128 * 180 / 140 */
bool media_mount(void);
void media_refresh(void);  /* force a new mount and root-directory scan */
bool media_is_mounted(void);
const char *media_sd_status(void);
const char *media_photo_status(void);
bool media_select(int direction,char *name,size_t size);
bool media_open(char *preview,size_t len);
bool media_photo(void);
const uint16_t *media_bitmap(void);
bool media_has_bitmap(void);
int media_index(void);
int media_count(void);
