// Standard Libraries
#include <stdio.h>
#include <stdint.h>

// ESP-IDF Headers
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_err.h"

// FreeRTOS Headers
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Project Headers
#include "sdkconfig.h"
#include "chargelogic.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    // Handle NVS edge cases
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS initialized");

    // Initialize internal charger module resources 
    charger_init();
    // Initialize peripherals and network stack
    wifi_init_sta();
    led_init();
    //mqtt_init();

    // Create tasks
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_task, "mqtt_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System initialization complete");
}