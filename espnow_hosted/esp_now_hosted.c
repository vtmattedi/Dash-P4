// ESP-NOW for a host without a radio (the ESP32-P4), carried out by the ESP-Hosted co-processor (the C6).
//
// esp_wifi_remote declares the esp_now_* API on the host but implements none of it. These are the calls an
// ESP-NOW client needs (NightMareNetwork's EspNowClient uses exactly these), each forwarded to the C6 as a
// request over ESP-Hosted's peer-data channel and answered with the C6's own esp_err_t. Received frames and
// send completions come back as events and reach the registered callbacks on ESP-Hosted's RPC receive task,
// where the native ones would run on the Wi-Fi task: keep them short, as with native ESP-NOW.
//
// The C6 must run the dash-p4-c6 firmware (ESP-Hosted slave + the nm_espnow bridge). With any other
// co-processor firmware esp_now_init() times out and returns ESP_ERR_NOT_SUPPORTED, and nothing else changes.
// Wire format: nm_espnow_rpc.h.

#include <string.h>

#include "esp_check.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nm_espnow_rpc.h"

static const char *TAG = "espnow_hosted";

// A request is a few hundred microseconds over SDIO plus the C6's work; INIT also starts ESP-NOW there.
#define CALL_TIMEOUT_MS 300
#define INIT_TIMEOUT_MS 800

static SemaphoreHandle_t s_call_lock; // one request in flight
static SemaphoreHandle_t s_reply;     // given by the reply to the request in flight
static volatile uint8_t s_wait_seq;
static volatile int32_t s_reply_err;
static volatile uint8_t s_reply_version;
static uint8_t s_seq;
static bool s_listening;

static esp_now_recv_cb_t s_recv_cb;
static esp_now_send_cb_t s_send_cb;

static void on_coprocessor(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    if (len < sizeof(nmen_head_t)) {
        return;
    }
    switch (data[0]) {
    case NMEN_EV_REPLY: {
        if (len < sizeof(nmen_reply_t)) {
            return;
        }
        nmen_reply_t reply;
        memcpy(&reply, data, sizeof(reply));
        if (reply.head.seq == s_wait_seq) {
            s_reply_err = reply.err;
            s_reply_version = reply.version;
            xSemaphoreGive(s_reply);
        }
        return;
    }
    case NMEN_EV_RECV: {
        if (len < sizeof(nmen_head_t) + sizeof(nmen_recv_t)) {
            return;
        }
        nmen_recv_t ev;
        memcpy(&ev, data + sizeof(nmen_head_t), sizeof(ev));
        const uint8_t *payload = data + sizeof(nmen_head_t) + sizeof(nmen_recv_t);
        if (ev.len > len - sizeof(nmen_head_t) - sizeof(nmen_recv_t)) {
            return;
        }
        esp_now_recv_cb_t cb = s_recv_cb;
        if (cb) {
            wifi_pkt_rx_ctrl_t rx_ctrl;
            memset(&rx_ctrl, 0, sizeof(rx_ctrl));
            rx_ctrl.rssi = ev.rssi;
            rx_ctrl.channel = ev.channel;
            esp_now_recv_info_t info = {
                .src_addr = ev.src,
                .des_addr = ev.dst,
                .rx_ctrl = &rx_ctrl,
            };
            cb(&info, payload, ev.len);
        }
        return;
    }
    case NMEN_EV_SENT: {
        if (len < sizeof(nmen_head_t) + sizeof(nmen_sent_t)) {
            return;
        }
        nmen_sent_t ev;
        memcpy(&ev, data + sizeof(nmen_head_t), sizeof(ev));
        esp_now_send_cb_t cb = s_send_cb;
        if (cb) {
            esp_now_send_info_t info;
            memset(&info, 0, sizeof(info));
            info.des_addr = ev.mac;
            info.ifidx = WIFI_IF_STA;
            info.tx_status = ev.status == 0 ? WIFI_SEND_SUCCESS : WIFI_SEND_FAIL;
            cb(&info, ev.status == 0 ? ESP_NOW_SEND_SUCCESS : ESP_NOW_SEND_FAIL);
        }
        return;
    }
    default:
        return;
    }
}

static esp_err_t ensure_listening(void)
{
    if (s_listening) {
        return ESP_OK;
    }
    if (!s_call_lock) {
        s_call_lock = xSemaphoreCreateMutex();
        s_reply = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_call_lock && s_reply, ESP_ERR_NO_MEM, TAG, "semaphores");
    }
    ESP_RETURN_ON_ERROR(esp_hosted_register_custom_callback(NMEN_MSG_TO_HOST, on_coprocessor), TAG,
                        "register the co-processor callback");
    s_listening = true;
    return ESP_OK;
}

/// Send one request and wait for its reply. `body` follows the [op][seq] head.
static esp_err_t call(uint8_t op, const void *body, size_t body_len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(ensure_listening(), TAG, "listen");
    uint8_t msg[sizeof(nmen_head_t) + NMEN_MAC_LEN + NMEN_MAX_DATA];
    ESP_RETURN_ON_FALSE(body_len <= sizeof(msg) - sizeof(nmen_head_t), ESP_ERR_INVALID_SIZE, TAG, "request too long");

    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    const uint8_t seq = ++s_seq;
    msg[0] = op;
    msg[1] = seq;
    if (body_len) {
        memcpy(msg + sizeof(nmen_head_t), body, body_len);
    }
    xSemaphoreTake(s_reply, 0); // a late reply to a request that timed out
    s_wait_seq = seq;
    esp_err_t err = esp_hosted_send_custom_data(NMEN_MSG_TO_COPROC, msg, sizeof(nmen_head_t) + body_len);
    if (err == ESP_OK) {
        err = xSemaphoreTake(s_reply, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? (esp_err_t)s_reply_err : ESP_ERR_TIMEOUT;
    }
    s_wait_seq = 0;
    xSemaphoreGive(s_call_lock);
    return err;
}

static void to_wire(const esp_now_peer_info_t *peer, nmen_peer_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, peer->peer_addr, NMEN_MAC_LEN);
    memcpy(out->lmk, peer->lmk, NMEN_KEY_LEN);
    out->channel = peer->channel;
    out->ifidx = (uint8_t)peer->ifidx;
    out->encrypt = peer->encrypt ? 1 : 0;
}

esp_err_t esp_now_init(void)
{
    const uint8_t version = NMEN_RPC_VERSION;
    esp_err_t err = call(NMEN_OP_INIT, &version, sizeof(version), INIT_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "the co-processor does not answer: it needs the dash-p4-c6 firmware for ESP-NOW");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && s_reply_version != NMEN_RPC_VERSION) {
        ESP_LOGE(TAG, "co-processor speaks ESP-NOW bridge v%u, this host v%u: update the one that is older",
                 s_reply_version, NMEN_RPC_VERSION);
        call(NMEN_OP_DEINIT, NULL, 0, CALL_TIMEOUT_MS);
        return ESP_ERR_INVALID_VERSION;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW on the co-processor (bridge v%u)", s_reply_version);
    }
    return err;
}

esp_err_t esp_now_deinit(void)
{
    return call(NMEN_OP_DEINIT, NULL, 0, CALL_TIMEOUT_MS);
}

esp_err_t esp_now_set_pmk(const uint8_t *pmk)
{
    ESP_RETURN_ON_FALSE(pmk, ESP_ERR_INVALID_ARG, TAG, "pmk");
    return call(NMEN_OP_SET_PMK, pmk, NMEN_KEY_LEN, CALL_TIMEOUT_MS);
}

esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer)
{
    ESP_RETURN_ON_FALSE(peer, ESP_ERR_INVALID_ARG, TAG, "peer");
    nmen_peer_t wire;
    to_wire(peer, &wire);
    return call(NMEN_OP_ADD_PEER, &wire, sizeof(wire), CALL_TIMEOUT_MS);
}

esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer)
{
    ESP_RETURN_ON_FALSE(peer, ESP_ERR_INVALID_ARG, TAG, "peer");
    nmen_peer_t wire;
    to_wire(peer, &wire);
    return call(NMEN_OP_MOD_PEER, &wire, sizeof(wire), CALL_TIMEOUT_MS);
}

esp_err_t esp_now_del_peer(const uint8_t *peer_addr)
{
    ESP_RETURN_ON_FALSE(peer_addr, ESP_ERR_INVALID_ARG, TAG, "peer address");
    return call(NMEN_OP_DEL_PEER, peer_addr, NMEN_MAC_LEN, CALL_TIMEOUT_MS);
}

esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data && len > 0 && len <= NMEN_MAX_DATA, ESP_ERR_INVALID_ARG, TAG, "data");
    uint8_t body[NMEN_MAC_LEN + NMEN_MAX_DATA];
    // A NULL address means "every peer" natively; the bridge treats an all-zero address the same way.
    if (peer_addr) {
        memcpy(body, peer_addr, NMEN_MAC_LEN);
    } else {
        memset(body, 0, NMEN_MAC_LEN);
    }
    memcpy(body + NMEN_MAC_LEN, data, len);
    return call(NMEN_OP_SEND, body, NMEN_MAC_LEN + len, CALL_TIMEOUT_MS);
}

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb)
{
    s_recv_cb = cb;
    return ensure_listening();
}

esp_err_t esp_now_unregister_recv_cb(void)
{
    s_recv_cb = NULL;
    return ESP_OK;
}

esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb)
{
    s_send_cb = cb;
    return ensure_listening();
}

esp_err_t esp_now_unregister_send_cb(void)
{
    s_send_cb = NULL;
    return ESP_OK;
}
