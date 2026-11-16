#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MEDIA_BMP_SIZE 96
bool media_mount(void);
bool media_select(int direction,char *name,size_t size);
bool media_open(char *preview,size_t len);
bool media_photo(void);
const uint16_t *media_bitmap(void);
bool media_has_bitmap(void);
