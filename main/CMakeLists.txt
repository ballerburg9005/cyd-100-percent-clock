# The public contact_details.txt contains placeholders. For local builds,
# contact_details.private.txt (gitignored) supersedes it. Generated headers
# are placed only in build/, never in Git.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CONTACT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/contact_generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_DIR}/contact_details.txt"
    "${PROJECT_DIR}/contact_details.private.txt"
    "${PROJECT_DIR}/face.bmp"
    "${PROJECT_DIR}/tools/generate_contact_assets.py")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_DIR}/tools/generate_contact_assets.py"
            "${PROJECT_DIR}" "${CONTACT_GENERATED_DIR}"
    RESULT_VARIABLE contact_result
    ERROR_VARIABLE contact_error
    OUTPUT_VARIABLE contact_output
)
if(NOT contact_result EQUAL 0)
    message(FATAL_ERROR "Contact asset generator failed: ${contact_error}")
endif()
message(STATUS "${contact_output}")

idf_component_register(
    SRCS "main.c" "screen.c" "media.c" "audio.c" "tv_video.c"
    INCLUDE_DIRS "." "${CONTACT_GENERATED_DIR}"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
             esp_http_server esp_lcd driver lwip fatfs sdmmc esp_driver_spi esp_driver_sdspi
             esp_driver_dac esp_driver_gptimer esp32_composite_video_lib
)
