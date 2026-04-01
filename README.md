# Charging Point Status Reporter
## Overview
This project implements a simulated EV charge point status reporter on the ESP32 using ESP-IDF and FreeRTOS.
The system includes hardware (schematic and PCB designs) and firmware. 

## Hardware
The system hardware was designed on `Easy-EDA (Pro)` using ESP32-WROOM-32 controller. The modules of the system include
power supply module, controller module (ESP32 module) and RGB driver module. 

### Power Supply
The systems is powered from an external `5 V` DC source. The 5 V directly powers the USB and the ESP-32 module through
a `3.3 V` voltage regulator. 
The ESP32-WROOM-32 Module requires DC supply voltage of `3.3 V` and current of not less that `500mA`. Given this specification,
the `AMS117-3.3 V` voltage regulator was used as it offers a fixed output of `3.3 V` and current up to `1 A`. 
Therefore, the power supply module consists of DC jack for power input connection, and LDO regulator to provide 3.3 V to the ESP32 module. 
The LDO used was the `TO-252` package where a `10uF` decoupling capacitor was included on the input to reduce noise and suppress voltage spikes
and a `22uF` capacitor to ensure stable operations. 

### ESP32 Module
The ESP32-WROOM-32 module minimal circuit comprises of the power supply, USB-C-to-UART, reset and enable peripheral schematics to get the module ready for operation.
The peripheral wiring was guided by ESP32 circuit schematics. The USB-to-UART hardware waw meant for downloading the firmware into the ESP32 controller. A type-C USB
was used since it is the local and international standard adopted. 

### RGB LED Driver
RGB LED contains three LEDs: Red, Green and Blue in commonn anode configuration. Each of the LEDs has different physical properties.
To control the RGB LED there is need for current limiting and driver for PWM dimming. Current limiting resistors of `220 Ω` were connected
to each LED leg of the RGB as well as to the Gate terminal of the mosfets. The `IRLZ44N` mosfets were used to drive the LEDs because
- The IRLZ44N is a logic-level MOSFET meaning that it can be controlled using microcontroller pin output voltage.
- The IRLZ44N has Drain-to-Source On-Resistance of `22 mΩ` resulting to low power dissipation hence low heat produced and requires no heat sink.

### Circuit Schematic
![CHARGING_POINT_STATUS_REPORTER](https://github.com/user-attachments/assets/e7520c73-e61d-475b-83f0-65ceb3a2af75)


### PCB Design
The PCB is two-layered PCB with ground copper pour on both top and bottom layers. The PCB tracks are of different widths where power and ground routes have `30 mil` width, 
signal routes have `10 mil` and other with `15 mil`.
#### Top Layer
<img width="546" height="383" alt="TOP PCB" src="https://github.com/user-attachments/assets/c01626e1-6457-4a35-999b-d376635c6f1a" />


#### Bottom Layer
<img width="657" height="458" alt="BOTTOM PCB" src="https://github.com/user-attachments/assets/5b1ea3e2-1ea6-47a7-8dd6-d33737411602" />


#### 3D Design
<img width="2160" height="1890" alt="CHARGE POINT 3D PCB" src="https://github.com/user-attachments/assets/667e2b51-f41c-47d0-9702-8bb7f97bb2f2" />


## Firmware
The firmware is designed to perform the following tasks:
- Connect to Wi-Fi network.
- Establish an MQTT connection.
- Simulate charger electrical parameters: current, voltage and uptime.
- Track charger operating state and publish periodically.
- Accept remote control commands
- Indicate the charging state using RGB LED.
The firmware was built using `ESP_IDF` using  FreeRTOs.

## Firmware Summary

At startup, the firmware initializes non-volatile storage (NVS), creates internal FreeRTOS queues, configures the RGB LED, and starts the Wi-Fi station interface.

If no Wi-Fi credentials are found in NVS, the firmware falls back to default credentials configured at build time through ESP-IDF configuration.
Once the ESP32 successfully connects to the network and receives an IP address, the MQTT client is initialized and started. This is important because it ensures the
application behaves in an event-driven way rather than trying to publish before the network is ready.

After connectivity is established, the firmware continuously simulates charger telemetry: voltage, current, uptime, and charger operating state. These values are periodically packaged into JSON format and published to an MQTT broker. At the same time, the device listens for remote MQTT commands that can control charger operation or update configuration.

The charger logic is built around a state machine with three operating states: **IDLE**, **CHARGING**, and **FAULT**.

## Charger State Behavior

The system starts in the **IDLE** state. In this condition, the charger is considered available but not actively charging. Current remains at zero, telemetry continues to be published, and the `Blue` Led blinks at 2 Hz.

When a valid remote start command is received, the system transitions into the **CHARGING** state. In this state, simulated charging current becomes non-zero, telemetry reflects active operation, and the `Green` LED blinks at 2 Hz

If the simulated electrical conditions exceed safe limits, the firmware automatically transitions into the **FAULT** state. In fault mode, charging current is forced to zero, the system continues publishing fault telemetry, and the `Red` LED blinks at 2 Hz. 

## Software Architecture
The firmware is organized around a modular task-based design using FreeRTOS.

The `main.c` file is responsible for system startup, NVS initialization, queue creation, and launching the application services.

The main application logic is implemented in `chargelogic.c`, which contains the Wi-Fi setup, MQTT setup, charger state machine, sensor simulation, LED control, event handling, and queue communication.

The corresponding `chargelogic.h` header defines the charger states, data structures, queue access functions, and task/function prototypes.

## Task and Queue Design

The **sensor task** is responsible for generating simulated voltage and current readings, updating the charger state, detecting faults, logging local charger behavior, and pushing fresh status data into the status queue.

The **MQTT task** is responsible for reading charger status from the queue, formatting it as JSON, and publishing it to the MQTT broker.

A **status queue** is used to transfer charger telemetry between the simulation logic and the MQTT publishing logic, while a **command queue** is used to move incoming remote commands into the charger control logic.

## Networking and MQTT

The ESP32 operates in Wi-Fi station mode and connects to a configured access point. Credentials can be loaded from NVS if previously saved, or default to values configured during build time.

The firmware uses MQTT for telemetry and remote control. The MQTT client is only started after Wi-Fi connection succeeds, preventing connection attempts before DNS and network services are available.

The project was tested using the **HiveMQ public broker**, and remote commands were sent using **MQTT Explorer** as the MQTT client during validation.

## Telemetry Format

The firmware periodically (5 Seconds) publishes structured JSON telemetry containing the charger’s identity, uptime, simulated voltage, simulated current, and current operating state.

A typical telemetry payload follows this format:

```json
{
  "device_id": "CHARGER01",
  "uptime_s": 40,
  "voltage_V": 229.62,
  "current_A": 5.44,
  "charge_state": "CHARGING"
}
