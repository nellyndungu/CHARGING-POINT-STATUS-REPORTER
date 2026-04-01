idf_component_register(
    SRCS "chargelogic.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_event nvs_flash mqtt json 
)