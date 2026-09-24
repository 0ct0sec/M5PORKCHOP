#include "pigsync_server.h"
#include "pigsync_protocol.h"

#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

#include "../core/network_recon.h"
#include "../ui/display.h"

bool PigSyncServerMode::running = false;
bool PigSyncServerMode::initialized = false;
bool PigSyncServerMode::connected = false;
uint8_t PigSyncServerMode::channel = PIGSYNC_DISCOVERY_CHANNEL;
uint8_t PigSyncServerMode::clientMac[6] = {};
uint16_t PigSyncServerMode::sessionId = 0;
uint8_t PigSyncServerMode::txSeq = 0;
uint32_t PigSyncServerMode::rxCount = 0;
uint32_t PigSyncServerMode::txCount = 0;
uint32_t PigSyncServerMode::lastActivity = 0;
char PigSyncServerMode::status[40] = "OFFLINE";

namespace {
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool packetPending = false;
uint8_t pendingMac[6] = {};
uint8_t pendingData[250] = {};
int pendingLen = 0;

void onServerRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len < (int)sizeof(PigSyncHeader) || len > (int)sizeof(pendingData)) return;
    const PigSyncHeader* hdr = reinterpret_cast<const PigSyncHeader*>(data);
    if (hdr->magic != PIGSYNC_MAGIC || hdr->version != PIGSYNC_VERSION) return;
    taskENTER_CRITICAL(&rxMux);
    if (!packetPending) {
        memcpy(pendingMac, mac, 6);
        memcpy(pendingData, data, len);
        pendingLen = len;
        packetPending = true;
    }
    taskEXIT_CRITICAL(&rxMux);
}

void onServerSent(const uint8_t*, esp_now_send_status_t) {}

bool addPeer(const uint8_t* mac, uint8_t peerChannel, bool encrypted) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = peerChannel;
    peer.encrypt = encrypted;
    if (encrypted) memcpy(peer.lmk, PIGSYNC_LMK, sizeof(peer.lmk));
    return esp_now_add_peer(&peer) == ESP_OK;
}

void fillHeader(PigSyncHeader& out, uint8_t type, uint8_t seq, uint8_t ack, uint16_t sid) {
    out = {};
    out.magic = PIGSYNC_MAGIC;
    out.version = PIGSYNC_VERSION;
    out.type = type;
    out.seq = seq;
    out.ack = ack;
    out.sessionId = sid;
}
} // namespace

void PigSyncServerMode::start() {
    if (running) return;
    NetworkRecon::pause();
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    delay(100);
    esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        strncpy(status, "ESP-NOW INIT FAILED", sizeof(status) - 1);
        return;
    }
    esp_now_set_pmk(PIGSYNC_PMK);
    esp_now_register_recv_cb(onServerRecv);
    esp_now_register_send_cb(onServerSent);
    initialized = true;
    running = true;
    connected = false;
    channel = PIGSYNC_DISCOVERY_CHANNEL;
    sessionId = 0;
    txSeq = 0;
    rxCount = txCount = 0;
    packetPending = false;
    strncpy(status, "LISTENING FOR POPS", sizeof(status) - 1);
}

void PigSyncServerMode::stop() {
    running = false;
    connected = false;
    if (initialized) {
        esp_now_deinit();
        initialized = false;
    }
    NetworkRecon::start();
}

void PigSyncServerMode::update() {
    if (!running || !packetPending) return;

    uint8_t mac[6];
    uint8_t data[250];
    int len;
    taskENTER_CRITICAL(&rxMux);
    memcpy(mac, pendingMac, 6);
    len = pendingLen;
    memcpy(data, pendingData, len);
    packetPending = false;
    taskEXIT_CRITICAL(&rxMux);

    const PigSyncHeader* hdr = reinterpret_cast<const PigSyncHeader*>(data);
    rxCount++;
    lastActivity = millis();

    if (hdr->type == CMD_DISCOVER && len >= (int)sizeof(CmdDiscover)) {
        addPeer(mac, PIGSYNC_DISCOVERY_CHANNEL, false);
        RspBeacon rsp = {};
        fillHeader(rsp.hdr, RSP_BEACON, ++txSeq, hdr->seq, 0);
        WiFi.macAddress(rsp.son_mac);
        rsp.pending = 0;
        rsp.flags = 0;
        rsp.rssi = 0;
        if (esp_now_send(mac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
        strncpy(status, "POPS DISCOVERED", sizeof(status) - 1);
        return;
    }

    if (hdr->type == CMD_HELLO) {
        memcpy(clientMac, mac, 6);
        addPeer(clientMac, PIGSYNC_DISCOVERY_CHANNEL, false);
        sessionId = static_cast<uint16_t>(esp_random() & 0xffffU);
        if (sessionId == 0) sessionId = 1;
        channel = selectDataChannel(sessionId);
        RspHello rsp = {};
        fillHeader(rsp.hdr, RSP_HELLO, ++txSeq, hdr->seq, sessionId);
        rsp.dialogue_id = sessionId % DIALOGUE_TRACK_COUNT;
        rsp.mood = 128;
        rsp.data_channel = channel;
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
        delay(25);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        addPeer(clientMac, channel, true);
        strncpy(status, "WAITING FOR READY", sizeof(status) - 1);
        return;
    }

    if (sessionId == 0 || hdr->sessionId != sessionId || memcmp(mac, clientMac, 6) != 0) return;

    if (hdr->type == CMD_READY) {
        RspReady rsp = {};
        fillHeader(rsp.hdr, RSP_READY, ++txSeq, hdr->seq, sessionId);
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
        connected = true;
        strncpy(status, "CONNECTED - NO CAPTURES", sizeof(status) - 1);
    } else if (hdr->type == CMD_GET_COUNT) {
        RspCount rsp = {};
        fillHeader(rsp.hdr, RSP_COUNT, ++txSeq, hdr->seq, sessionId);
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
    } else if (hdr->type == CMD_TIME_SYNC && len >= (int)sizeof(CmdTimeSync)) {
        const CmdTimeSync* cmd = reinterpret_cast<const CmdTimeSync*>(data);
        RspTimeSync rsp = {};
        fillHeader(rsp.hdr, RSP_TIME_SYNC, ++txSeq, hdr->seq, sessionId);
        rsp.echoedMillis = cmd->porkchopMillis;
        time_t now = time(nullptr);
        rsp.rtcValid = now > 1600000000;
        rsp.sirloinUnixTime = rsp.rtcValid ? static_cast<uint32_t>(now) : 0;
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
    } else if (hdr->type == CMD_BOUNTIES && len >= (int)sizeof(CmdBounties)) {
        const CmdBounties* cmd = reinterpret_cast<const CmdBounties*>(data);
        RspBountiesAck rsp = {};
        fillHeader(rsp.hdr, RSP_BOUNTIES_ACK, ++txSeq, hdr->seq, sessionId);
        rsp.count = cmd->count;
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
    } else if (hdr->type == CMD_PURGE) {
        RspPurged rsp = {};
        fillHeader(rsp.hdr, RSP_PURGED, ++txSeq, hdr->seq, sessionId);
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
    } else if (hdr->type == CMD_START_SYNC) {
        RspError rsp = {};
        fillHeader(rsp.hdr, RSP_ERROR, ++txSeq, hdr->seq, sessionId);
        rsp.error_code = PIGSYNC_ERR_NO_CAPTURES;
        if (esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp)) == ESP_OK) txCount++;
    } else if (hdr->type == CMD_DISCONNECT || hdr->type == CMD_ABORT) {
        PigSyncHeader rsp = {};
        fillHeader(rsp, RSP_DISCONNECT, ++txSeq, hdr->seq, sessionId);
        esp_now_send(clientMac, reinterpret_cast<uint8_t*>(&rsp), sizeof(rsp));
        txCount++;
        delay(20);
        esp_now_del_peer(clientMac);
        esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL, WIFI_SECOND_CHAN_NONE);
        channel = PIGSYNC_DISCOVERY_CHANNEL;
        sessionId = 0;
        connected = false;
        strncpy(status, "LISTENING FOR POPS", sizeof(status) - 1);
    }
}

void PigSyncServerMode::draw(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.drawString("SIRLOIN", canvas.width() / 2, 10);
    canvas.setTextSize(1);
    canvas.drawString(status, canvas.width() / 2, 40);
    char line[64];
    snprintf(line, sizeof(line), "CH:%u  RX:%lu  TX:%lu", channel,
             (unsigned long)rxCount, (unsigned long)txCount);
    canvas.drawString(line, canvas.width() / 2, 58);
    canvas.drawString(connected ? "ENCRYPTED SESSION ACTIVE" : "DISCOVERY CHANNEL 1", canvas.width() / 2, 74);
    canvas.drawString("[ESC] STOP SERVER", canvas.width() / 2, 94);
}
