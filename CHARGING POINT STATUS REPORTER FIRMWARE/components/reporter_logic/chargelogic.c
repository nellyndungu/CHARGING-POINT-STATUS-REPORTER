// Standard Libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Project Headers
#include "chargelogic.h"
#include "sdkconfig.h"

// ESP-IDF Headers
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

// FreeRTOS Headers
#include "freertos/task.h"
#include "freertos/queue.h"

// LED Configuration
#define LED_RED    CONFIG_LED_RED_GPIO
#define LED_GREEN  CONFIG_LED_GREEN_GPIO
#define LED_BLUE   CONFIG_LED_BLUE_GPIO

// NVS Keys
#define NVS_NAMESPACE      "wifi_cfg"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASSWORD   "password"

// Define variables
static bool led_on = false;
static esp_timer_handle_t led_timer;
static charge_state_t led_state = STATE_IDLE;
static bool mqtt_started = false;

static char topic_status[64];
static char topic_cmd[64];
static char topic_lwt[64];
static char topic_config[64];

static QueueHandle_t status_queue = NULL;
static QueueHandle_t cmd_queue = NULL;

static esp_mqtt_client_handle_t mqtt_client = NULL;

static const char *TAG = "CHARGER";

//Function for updating logs
static const char *state_to_string(charge_state_t state)
{
    switch (state) {
        case STATE_IDLE: return "IDLE";
        case STATE_CHARGING: return "CHARGING";
        case STATE_FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

static void led_update_state(charge_state_t state)
{
    led_state = state;
}

static void set_charge_state(charge_status_t *status, charge_state_t new_state)
{
    if (status->state != new_state) {
        ESP_LOGI(TAG, "State transition: %s -> %s",
                 state_to_string(status->state),
                 state_to_string(new_state));

        status->state = new_state;
        led_update_state(new_state);
    }
}

// Queue Access Functions
QueueHandle_t charger_get_status_queue(void)
{
    return status_queue;
}

QueueHandle_t charger_get_cmd_queue(void)
{
    return cmd_queue;
}

// NVS Wi-Fi Credential Functions
static esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi credentials saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save Wi-Fi credentials");
    }

    return err;
}

static bool wifi_load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved Wi-Fi credentials found in NVS");
        return false;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG, "Saved SSID not found in NVS");
        return false;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &password_len);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved Wi-Fi password not found in NVS");
        return false;
    }

    ESP_LOGI(TAG, "Loaded Wi-Fi credentials from NVS");
    return true;
}

// Wi-Fi Runtime Reconfiguration
void wifi_reconnect_with_new_credentials(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "Applying new Wi-Fi credentials...");
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

// Module Initialization
void charger_init(void)
{
    status_queue = xQueueCreate(5, sizeof(charge_status_t));
    cmd_queue = xQueueCreate(5, sizeof(charger_cmd_t));

    if (status_queue == NULL || cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS queues");
    } else {
        ESP_LOGI(TAG, "Queues initialized successfully");
    }
}

// Wi-Fi Event Handler
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started, connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Wi-Fi connected. IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        
        if (!mqtt_started) {
        mqtt_init();
        mqtt_started = true;
        ESP_LOGI(TAG, "MQTT started after Wi-Fi connection");
    }
    }
}

// Wi-Fi Initialization
void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip));

    // Try loading credentials from NVS first
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};

    wifi_config_t wifi_config = {0};

    if (wifi_load_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password))) {
        strncpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, saved_password, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Using saved Wi-Fi credentials from NVS");
    } else {
        strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Using default Wi-Fi credentials from Kconfig");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialized in station mode");
}

// LED Timer Callback (2 Hz Blink)
static void led_timer_callback(void *args)
{
    led_on = !led_on;

    gpio_set_level(LED_RED, 0);
    gpio_set_level(LED_GREEN, 0);
    gpio_set_level(LED_BLUE, 0);

    if (!led_on) {
        return;
    }

    switch (led_state) {
        case STATE_IDLE:
            gpio_set_level(LED_BLUE, 1);
            break;

        case STATE_CHARGING:
            gpio_set_level(LED_GREEN, 1);
            break;

        case STATE_FAULT:
            gpio_set_level(LED_RED, 1);
            break;

        default:
            break;
    }
}

// LED Initialization
void led_init(void)
{
    gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_RED, 0);
    gpio_set_level(LED_GREEN, 0);
    gpio_set_level(LED_BLUE, 0);

    const esp_timer_create_args_t timer_args = {
        .callback = &led_timer_callback,
        .name = "led_timer"
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, 250000));

    ESP_LOGI(TAG, "RGB LED initialized");
}

// Sensor Simulation Task
void sensor_task(void *arg)
{
    charge_status_t status = {0};
    charger_cmd_t cmd;

    status.state = STATE_IDLE;
    status.voltage = 230.0f;
    status.current = 0.0f;
    status.uptime = 0;

    led_update_state(status.state);

    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        if (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case START:
                    if (status.state != STATE_FAULT) {
                        ESP_LOGI(TAG, "Received command: START_CHARGE");
                        set_charge_state(&status, STATE_CHARGING);
                    } else {
                        ESP_LOGW(TAG, "START_CHARGE ignored: charger is in FAULT state");
                    }
                    break;

                case STOP:
                    ESP_LOGI(TAG, "Received command: STOP_CHARGE");
                    set_charge_state(&status, STATE_IDLE);
                    break;

                default:
                    break;
            }
        }

        status.uptime += (CONFIG_PUBLISH_INTERVAL_MS / 1000);

        float step = (((float) rand() / RAND_MAX) * 2.0f) - 1.0f;
        status.voltage += step;

        if (status.voltage < (CONFIG_VOLTAGE_MIN - 2)) status.voltage = (CONFIG_VOLTAGE_MIN - 2);
        if (status.voltage > (CONFIG_VOLTAGE_MAX + 2)) status.voltage = (CONFIG_VOLTAGE_MAX + 2);

        if (status.voltage < CONFIG_VOLTAGE_MIN || status.voltage > CONFIG_VOLTAGE_MAX) {
            set_charge_state(&status, STATE_FAULT);
        }

        if (status.state == STATE_CHARGING) {
            status.current = ((float) rand() / RAND_MAX) * CONFIG_CURRENT_MAX;
        } else {
            status.current = 0.0f;
        }

        if (xQueueSend(status_queue, &status, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send status to queue");
        }

        ESP_LOGI(TAG,
                 "Simulated -> Uptime: %d s | Voltage: %.2f V | Current: %.2f A | State: %s",
                 status.uptime,
                 status.voltage,
                 status.current,
                 state_to_string(status.state));

        vTaskDelay(pdMS_TO_TICKS(CONFIG_PUBLISH_INTERVAL_MS));
    }
}

// MQTT Topic Builder
static void mqtt_build_topics(void)
{
    snprintf(topic_status, sizeof(topic_status),
             CONFIG_MQTT_TOPIC_STATUS,
             CONFIG_DEVICE_ID);

    snprintf(topic_cmd, sizeof(topic_cmd),
             CONFIG_MQTT_TOPIC_CMD,
             CONFIG_DEVICE_ID);

    snprintf(topic_lwt, sizeof(topic_lwt),
             CONFIG_MQTT_TOPIC_LWT,
             CONFIG_DEVICE_ID);

    snprintf(topic_config, sizeof(topic_config),
             CONFIG_MQTT_TOPIC_CONFIG,
             CONFIG_DEVICE_ID);
}

// Wi-Fi Config JSON Parser
static void process_wifi_config_payload(const char *json_payload)
{
    cJSON *root = cJSON_Parse(json_payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid Wi-Fi config JSON received");
        return;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        ESP_LOGW(TAG, "Wi-Fi config JSON missing ssid or password");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Received new Wi-Fi config via MQTT");
    ESP_LOGI(TAG, "New SSID: %s", ssid->valuestring);

    if (wifi_save_credentials(ssid->valuestring, password->valuestring) == ESP_OK) {
        wifi_reconnect_with_new_credentials(ssid->valuestring, password->valuestring);
    }

    cJSON_Delete(root);
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");

            esp_mqtt_client_subscribe(mqtt_client, topic_cmd, 0);
            esp_mqtt_client_subscribe(mqtt_client, topic_config, 0);

            esp_mqtt_client_publish(
                mqtt_client,
                topic_lwt,
                "{\"status\":\"ONLINE\"}",
                0,
                1,
                1
            );

            ESP_LOGI(TAG, "Subscribed to command topic: %s", topic_cmd);
            ESP_LOGI(TAG, "Subscribed to Wi-Fi config topic: %s", topic_config);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
            char topic_buf[128] = {0};
            char data_buf[256] = {0};

            int topic_len = (event->topic_len < (int)(sizeof(topic_buf) - 1)) ? event->topic_len : (int)(sizeof(topic_buf) - 1);
            int data_len  = (event->data_len  < (int)(sizeof(data_buf)  - 1)) ? event->data_len  : (int)(sizeof(data_buf)  - 1);

            memcpy(topic_buf, event->topic, topic_len);
            topic_buf[topic_len] = '\0';

            memcpy(data_buf, event->data, data_len);
            data_buf[data_len] = '\0';

            ESP_LOGI(TAG, "MQTT message received on topic: %s", topic_buf);
            ESP_LOGI(TAG, "Payload: %s", data_buf);

            if (strcmp(topic_buf, topic_cmd) == 0) {
                charger_cmd_t cmd;
                QueueHandle_t q = charger_get_cmd_queue();

                if (strcmp(data_buf, "START_CHARGE") == 0) {
                    cmd = START;
                }
                else if (strcmp(data_buf, "STOP_CHARGE") == 0) {
                    cmd = STOP;
                }
                else {
                    ESP_LOGW(TAG, "Unknown charger command received");
                    break;
                }

                if (xQueueSend(q, &cmd, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Failed to queue charger command");
                }
            }
            else if (strcmp(topic_buf, topic_config) == 0) {
                process_wifi_config_payload(data_buf);
            }

            break;
        }

        default:
            break;
    }
}

// MQTT Initialization
void mqtt_init(void)
{
    mqtt_build_topics();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URL,

        .session.last_will.topic = topic_lwt,
        .session.last_will.msg = "{\"status\":\"OFFLINE\"}",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    esp_mqtt_client_register_event(
        mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG, "MQTT client initialized");
}

// MQTT Publish Task
void mqtt_task(void *arg)
{
    charge_status_t status;
    char payload[256];

    ESP_LOGI(TAG, "MQTT publish task started");

    while (1) {
        if (xQueueReceive(status_queue, &status, portMAX_DELAY) == pdTRUE) {

            snprintf(payload, sizeof(payload),
                     "{"
                     "\"device_id\":\"%s\","
                     "\"uptime_s\":%d,"
                     "\"voltage_V\":%.2f,"
                     "\"current_A\":%.2f,"
                     "\"charge_state\":\"%s\""
                     "}",
                     CONFIG_DEVICE_ID,
                     status.uptime,
                     status.voltage,
                     status.current,
                     state_to_string(status.state));

            ESP_LOGI(TAG, "Publishing status: %s", payload);

            if (mqtt_client != NULL) {
                esp_mqtt_client_publish(
                    mqtt_client,
                    topic_status,
                    payload,
                    0,
                    1,
                    0
                );
            }
        }
    }
}