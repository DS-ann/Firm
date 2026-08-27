# ESP32 Wi-Fi Internet Repeater

This branch contains a separate ESP-IDF project for using a classic ESP32 (NodeMCU-32S) as a small Wi-Fi internet-sharing gateway:

`Phone A hotspot -> ESP32 STA -> NAPT -> ESP32 AP -> Phone B`

## Framework

The project is pinned to **ESP-IDF v5.5.5**, a stable bug-fix release. Espressif recommends stable releases for production use; v5.5.5 was released on 2026-07-30.

## Configure

Run:

```bash
idf.py menuconfig
```

Then open **Wi-Fi Repeater** and set:

- Phone hotspot SSID
- Phone hotspot password
- ESP32 AP SSID
- ESP32 AP password
- Initial AP channel

The password for the ESP32 AP must be at least 8 characters.

The project enables lwIP IPv4 forwarding and NAPT in `sdkconfig.defaults`.

## Important Wi-Fi limitation

The classic ESP32 has one 2.4 GHz Wi-Fi radio. In AP+STA mode, the SoftAP follows the upstream STA channel. The phone providing the hotspot therefore needs a **2.4 GHz hotspot** for this project.

## Build

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

GitHub Actions builds the project automatically on pushes and pull requests to `wifi-repeater-espidf` using ESP-IDF v5.5.5.

## Behavior

- ESP32 starts its local AP immediately.
- STA connects to the phone hotspot.
- STA automatically reconnects after upstream loss.
- Once the STA has an IP, the AP receives the upstream DNS information.
- NAPT is enabled on the AP interface and the STA is the default upstream interface.
- Phone B can remain connected to the ESP32 AP while the upstream phone temporarily reconnects.

This is an internet-sharing gateway, not a transparent Layer-2 Wi-Fi bridge.
