# BLE hardening verification

The firmware uses NimBLE as a GATT server. The BLE implementation should be verified for connection/disconnection recovery, notification subscription state, multi-client handling, MTU-sized notification payloads, and Wi-Fi/BLE state transitions.

No daily usage reset, relay command parsing, or configuration/security changes are included in this note.