#ifndef COMPONENTS_REPORTER_LOGIC_CHARGELOGIC_H_
#define COMPONENTS_REPORTER_LOGIC_CHARGELOGIC_H_

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Charge state definitions
typedef enum {
    STATE_IDLE = 0,
    STATE_CHARGING,
    STATE_FAULT
} charge_state_t;

typedef enum {
    START = 0,
    STOP
} charger_cmd_t;

// Charger Status Structure
typedef struct {
    float voltage;
    float current;
    int uptime;
    charge_state_t state;
} charge_status_t;

// Queue Access Functions
QueueHandle_t charger_get_status_queue(void);
QueueHandle_t charger_get_cmd_queue(void);

// System Initialization
void charger_init(void);
void wifi_init_sta(void);
void mqtt_init(void);
void led_init(void);

// Wi-Fi runtime configuration
void wifi_reconnect_with_new_credentials(const char *ssid, const char *password);

// Sensor and MQTT tasks
void sensor_task(void *arg);
void mqtt_task(void *arg);

#endif 