# Firm — native ESP-IDF conversion

This branch contains the native ESP-IDF port of `Firm.ino`.

## Preserved behavior

- 8 active-low relays on GPIO 13, 4, 5, 18, 19, 21, 22, 23
- switch input on GPIO 33
- Wi-Fi/MQTT/BLE status LEDs on GPIO 25/26/27
- two 50 Hz fan servos on GPIO 14 and GPIO 32
- relay timers and daily usage accounting
- command protocol: `status`, `01`/`00` style relay commands, `T0<minutes>` timers and `F<room><speed>` fan commands
- MQTT topics used by the Arduino firmware
- Nordic-UART-style BLE service/characteristics and up to 5 tracked clients
- automatic Wi-Fi-to-BLE mode switching and periodic Wi-Fi scanning while BLE mode is idle

## Build

Use an ESP-IDF installation appropriate for the ESP32 target, then:

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py flash monitor
```

Set the four Wi-Fi SSID/password pairs and MQTT credentials under **Firm ESP-IDF configuration** in `menuconfig`.

## Security

The old Arduino source contained Wi-Fi and MQTT credentials. They are deliberately **not copied into this branch**. Rotate the credentials that were exposed in the old repository and enter the replacement values through `menuconfig`.

## Important migration note

This is a native ESP-IDF port, not an Arduino-as-a-component project. `PubSubClient`, `WiFi`, `ESP32Servo`, and `NimBLE-Arduino` are no longer required. Native ESP-IDF MQTT, Wi-Fi, LEDC and NimBLE APIs are used instead.

The BLE host is initialized once and advertising is enabled/disabled with the firmware state machine. This is safer than repeatedly destroying and recreating the NimBLE host.
