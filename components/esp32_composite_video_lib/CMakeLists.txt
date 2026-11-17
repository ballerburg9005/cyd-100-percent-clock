# Experimental GPL-3.0-or-later composite DAC/I2S0 driver by aquaticus.
# Upstream: https://github.com/aquaticus/esp32_composite_video_lib
# Snapshot main @ a1f5c7669aa274f148157947cdb7c94459e23777
# Adapted obsolete driver/periph_ctrl.h include for ESP-IDF 5.4.
# Do not include upstream LVGL adapter: clock uses native LCD renderer.
idf_component_register(
    SRCS "video.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_dac esp_driver_i2s esp_hw_support esp_timer
)
