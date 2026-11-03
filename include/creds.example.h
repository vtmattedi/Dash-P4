// Template for include/creds.h, which is gitignored because it holds secrets.
// Copy this file to include/creds.h and fill in the real values.
//
// Names match the older devices/Dashboard so one creds file serves both.

#pragma once

// Wi-Fi profile the panel uses when none is stored in its settings.
#define DEFAULT_SSID "your-ssid"
#define DEFAULT_PASSWORD "your-wifi-password"

// MQTT. The primary transport. Fill in the broker(s) you use:
//   REMOTE_* : TLS broker (needs ROOT_CA below)
//   LOCAL_*  : plain broker on the LAN
#define LOCAL_MQTT_HOST "192.168.0.10"
#define LOCAL_MQTT_PORT 1883
#define REMOTE_MQTT_URL "broker.example.com"
#define REMOTE_MQTT_PORT 8883
#define MQTT_USER "user"
#define MQTT_PASSWD "password"

// PEM of the CA that signed the remote broker's certificate.
#define ROOT_CA \
    "-----BEGIN CERTIFICATE-----\n" \
    "...\n" \
    "-----END CERTIFICATE-----\n"

// ESP-NOW network key (16-64 bytes), the same on the gateway and every device.
// Only used when NM_NETWORK_ESPNOW is 1 (see .local/plan.md P7).
#define NM_ESPNOW_PSK "change-me-to-a-16-to-64-byte-key"
