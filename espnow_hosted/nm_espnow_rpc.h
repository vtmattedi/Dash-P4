#pragma once

// ESP-NOW over ESP-Hosted: the wire between the P4 (host, espnow_hosted/esp_now_hosted.c in Dash-P4) and the
// ESP32-C6 co-processor (main/nm_espnow_bridge.c in dash-p4-c6).
//
// THIS FILE EXISTS TWICE, once in each project, and the two copies must stay identical. NMEN_RPC_VERSION is
// exchanged in the INIT reply; a mismatch fails esp_now_init() on the host with ESP_ERR_INVALID_VERSION.
//
// Transport: ESP-Hosted "peer data" (esp_hosted_send_custom_data / esp_hosted_register_custom_callback, both
// sides, up to 8166 bytes per message). One message id per direction; the first byte says what it is.
//
//   host -> C6   NMEN_MSG_TO_COPROC   [op][seq][body]            a request; every request gets one reply
//   C6 -> host   NMEN_MSG_TO_HOST     [NMEN_EV_REPLY][seq][int32 esp_err_t][uint8 version]
//                                     [NMEN_EV_RECV][0][nmen_recv_t][data]
//                                     [NMEN_EV_SENT][0][nmen_sent_t]
//
// Request bodies:
//   INIT       uint8 host version         C6: esp_now_init() + its own receive/send callbacks
//   DEINIT     -                          C6: esp_now_deinit()
//   SET_PMK    uint8 pmk[16]
//   ADD_PEER   nmen_peer_t
//   MOD_PEER   nmen_peer_t
//   DEL_PEER   uint8 mac[6]
//   SEND       uint8 mac[6] + data (the rest of the message, at most ESP_NOW_MAX_DATA_LEN = 250)
//
// The reply carries the C6's own esp_err_t (ESP_ERR_ESPNOW_EXIST and friends pass straight through). Receive
// and send-done events arrive asynchronously, in the order the C6's Wi-Fi task produced them. All integers
// little-endian; every struct packed.

#include <stdint.h>

#define NMEN_RPC_VERSION 1

#define NMEN_MSG_TO_COPROC 0x4E4D4531u // "NME1"
#define NMEN_MSG_TO_HOST 0x4E4D4532u   // "NME2"

#define NMEN_MAC_LEN 6
#define NMEN_KEY_LEN 16
#define NMEN_MAX_DATA 250

enum {
    NMEN_OP_INIT = 1,
    NMEN_OP_DEINIT = 2,
    NMEN_OP_SET_PMK = 3,
    NMEN_OP_ADD_PEER = 4,
    NMEN_OP_MOD_PEER = 5,
    NMEN_OP_DEL_PEER = 6,
    NMEN_OP_SEND = 7,

    NMEN_EV_REPLY = 0x80,
    NMEN_EV_RECV = 0x81,
    NMEN_EV_SENT = 0x82,
};

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t seq;
} nmen_head_t;

typedef struct __attribute__((packed)) {
    uint8_t mac[NMEN_MAC_LEN];
    uint8_t lmk[NMEN_KEY_LEN];
    uint8_t channel; // 0 = the station's current channel
    uint8_t ifidx;   // wifi_interface_t
    uint8_t encrypt;
} nmen_peer_t;

typedef struct __attribute__((packed)) {
    nmen_head_t head;
    int32_t err;
    uint8_t version;
} nmen_reply_t;

typedef struct __attribute__((packed)) {
    uint8_t src[NMEN_MAC_LEN];
    uint8_t dst[NMEN_MAC_LEN];
    int8_t rssi;
    uint8_t channel;
    uint16_t len; // data bytes following
} nmen_recv_t;

typedef struct __attribute__((packed)) {
    uint8_t mac[NMEN_MAC_LEN];
    uint8_t status; // esp_now_send_status_t: 0 success, 1 fail
} nmen_sent_t;
