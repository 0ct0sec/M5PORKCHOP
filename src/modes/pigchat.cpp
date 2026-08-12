#include "pigchat.h"

#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../core/config.h"
#include "../core/network_recon.h"
#include "../ui/display.h"

bool PigChatMode::running = false;

namespace {
constexpr uint8_t CHAT_MAGIC = 0xC7;
constexpr uint8_t CHAT_VERSION = 1;
constexpr uint8_t CHAT_CHANNEL = 1;
constexpr size_t MAX_PEERS = 8;
constexpr size_t MAX_TEXT = 96;
constexpr size_t HISTORY_SIZE = 8;
constexpr size_t RX_QUEUE_SIZE = 6;

enum PacketType : uint8_t {
    CHAT_DISCOVER = 1,
    CHAT_BEACON = 2,
    CHAT_HELLO = 3,
    CHAT_ACCEPT = 4,
    CHAT_MESSAGE = 5,
    CHAT_ACK = 6,
    CHAT_BYE = 7
};

#pragma pack(push, 1)
struct ChatHeader {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t seq;
    uint16_t sessionId;
    uint16_t messageId;
};
struct ChatNamedPacket {
    ChatHeader hdr;
    char name[16];
};
struct ChatMessagePacket {
    ChatHeader hdr;
    uint8_t textLen;
    char text[MAX_TEXT];
};
#pragma pack(pop)

struct PeerEntry {
    uint8_t mac[6];
    char name[16];
    uint32_t lastSeen;
};
struct HistoryEntry {
    bool mine;
    bool delivered;
    uint16_t id;
    char text[MAX_TEXT + 1];
};
struct RxSlot {
    bool used;
    uint8_t mac[6];
    uint8_t data[sizeof(ChatMessagePacket)];
    uint16_t len;
};

PeerEntry peers[MAX_PEERS] = {};
uint8_t peerCount = 0;
uint8_t selectedPeer = 0;
uint8_t remoteMac[6] = {};
char remoteName[16] = {};
bool connected = false;
bool connecting = false;
bool keyLatched = true;
uint16_t sessionId = 0;
uint16_t nextMessageId = 1;
uint8_t txSeq = 0;
uint32_t lastDiscover = 0;
uint32_t connectStarted = 0;
char input[MAX_TEXT + 1] = {};
uint8_t inputLen = 0;
HistoryEntry history[HISTORY_SIZE] = {};
uint8_t historyStart = 0;
uint8_t historyCount = 0;
RxSlot rxQueue[RX_QUEUE_SIZE] = {};
uint8_t rxWrite = 0;
uint8_t rxRead = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

const uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

const char* localName() {
    const char* configured = Config::personality().callsign;
    return configured[0] ? configured : "PIG";
}

void fillHeader(ChatHeader& hdr, uint8_t type, uint16_t sid = 0, uint16_t messageId = 0) {
    hdr = {};
    hdr.magic = CHAT_MAGIC;
    hdr.version = CHAT_VERSION;
    hdr.type = type;
    hdr.seq = ++txSeq;
    hdr.sessionId = sid;
    hdr.messageId = messageId;
}

bool addPeer(const uint8_t* mac, bool encrypt) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CHAT_CHANNEL;
    peer.encrypt = encrypt;
    if (encrypt) {
        static const uint8_t lmk[16] = {
            'P','I','G','C','H','A','T','-','L','M','K','-','2','0','2','6'
        };
        memcpy(peer.lmk, lmk, sizeof(lmk));
    }
    return esp_now_add_peer(&peer) == ESP_OK;
}

void sendNamed(const uint8_t* mac, uint8_t type, uint16_t sid = 0) {
    ChatNamedPacket packet = {};
    fillHeader(packet.hdr, type, sid);
    strncpy(packet.name, localName(), sizeof(packet.name) - 1);
    esp_now_send(mac, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
}

void addHistory(bool mine, const char* text, uint16_t id, bool delivered) {
    uint8_t slot;
    if (historyCount < HISTORY_SIZE) {
        slot = (historyStart + historyCount++) % HISTORY_SIZE;
    } else {
        slot = historyStart;
        historyStart = (historyStart + 1) % HISTORY_SIZE;
    }
    history[slot] = {};
    history[slot].mine = mine;
    history[slot].delivered = delivered;
    history[slot].id = id;
    strncpy(history[slot].text, text, MAX_TEXT);
}

void markDelivered(uint16_t id) {
    for (uint8_t i = 0; i < historyCount; i++) {
        HistoryEntry& entry = history[(historyStart + i) % HISTORY_SIZE];
        if (entry.mine && entry.id == id) entry.delivered = true;
    }
}

void rememberPeer(const uint8_t* mac, const char* name) {
    if (connected && memcmp(mac, remoteMac, 6) == 0) return;
    for (uint8_t i = 0; i < peerCount; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0) {
            strncpy(peers[i].name, name, sizeof(peers[i].name) - 1);
            peers[i].lastSeen = millis();
            return;
        }
    }
    if (peerCount >= MAX_PEERS) return;
    memcpy(peers[peerCount].mac, mac, 6);
    strncpy(peers[peerCount].name, name, sizeof(peers[peerCount].name) - 1);
    peers[peerCount].lastSeen = millis();
    peerCount++;
}

void onChatRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len < (int)sizeof(ChatHeader) || len > (int)sizeof(ChatMessagePacket)) return;
    const ChatHeader* hdr = reinterpret_cast<const ChatHeader*>(data);
    if (hdr->magic != CHAT_MAGIC || hdr->version != CHAT_VERSION) return;
    taskENTER_CRITICAL(&rxMux);
    uint8_t next = (rxWrite + 1) % RX_QUEUE_SIZE;
    if (next != rxRead) {
        RxSlot& slot = rxQueue[rxWrite];
        slot.used = true;
        memcpy(slot.mac, mac, 6);
        memcpy(slot.data, data, len);
        slot.len = len;
        rxWrite = next;
    }
    taskEXIT_CRITICAL(&rxMux);
}

void onChatSent(const uint8_t*, esp_now_send_status_t) {}

void disconnectPeer(bool notify) {
    if (connected && notify) {
        ChatHeader bye;
        fillHeader(bye, CHAT_BYE, sessionId);
        esp_now_send(remoteMac, reinterpret_cast<uint8_t*>(&bye), sizeof(bye));
        delay(15);
    }
    if (remoteMac[0] || remoteMac[1] || remoteMac[2]) esp_now_del_peer(remoteMac);
    connected = false;
    connecting = false;
    sessionId = 0;
    memset(remoteMac, 0, sizeof(remoteMac));
    memset(remoteName, 0, sizeof(remoteName));
}

void processPacket(const RxSlot& slot) {
    const ChatHeader* hdr = reinterpret_cast<const ChatHeader*>(slot.data);
    if (hdr->type == CHAT_DISCOVER && slot.len >= sizeof(ChatNamedPacket)) {
        // A discovery packet queued just before connection must not replace
        // the encrypted peer entry with an unencrypted one.
        if (connected || connecting) return;
        const ChatNamedPacket* packet = reinterpret_cast<const ChatNamedPacket*>(slot.data);
        rememberPeer(slot.mac, packet->name);
        addPeer(slot.mac, false);
        sendNamed(slot.mac, CHAT_BEACON);
        return;
    }
    if (hdr->type == CHAT_BEACON && slot.len >= sizeof(ChatNamedPacket)) {
        if (connected || connecting) return;
        const ChatNamedPacket* packet = reinterpret_cast<const ChatNamedPacket*>(slot.data);
        rememberPeer(slot.mac, packet->name);
        return;
    }
    if (hdr->type == CHAT_HELLO && slot.len >= sizeof(ChatNamedPacket) && !connected) {
        const ChatNamedPacket* packet = reinterpret_cast<const ChatNamedPacket*>(slot.data);
        memcpy(remoteMac, slot.mac, 6);
        strncpy(remoteName, packet->name, sizeof(remoteName) - 1);
        sessionId = hdr->sessionId ? hdr->sessionId : 1;
        addPeer(remoteMac, false);
        sendNamed(remoteMac, CHAT_ACCEPT, sessionId);
        delay(30);
        addPeer(remoteMac, true);
        connected = true;
        connecting = false;
        addHistory(false, "CONNECTED", 0, true);
        return;
    }
    if (hdr->type == CHAT_ACCEPT && connecting && hdr->sessionId == sessionId) {
        const ChatNamedPacket* packet = reinterpret_cast<const ChatNamedPacket*>(slot.data);
        strncpy(remoteName, packet->name, sizeof(remoteName) - 1);
        addPeer(remoteMac, true);
        connected = true;
        connecting = false;
        addHistory(false, "CONNECTED", 0, true);
        return;
    }
    if (!connected || hdr->sessionId != sessionId || memcmp(slot.mac, remoteMac, 6) != 0) return;
    if (hdr->type == CHAT_MESSAGE && slot.len >= sizeof(ChatHeader) + 1) {
        const ChatMessagePacket* packet = reinterpret_cast<const ChatMessagePacket*>(slot.data);
        uint8_t available = slot.len - sizeof(ChatHeader) - 1;
        uint8_t count = min(packet->textLen, available);
        if (count > MAX_TEXT) count = MAX_TEXT;
        char text[MAX_TEXT + 1] = {};
        memcpy(text, packet->text, count);
        addHistory(false, text, hdr->messageId, true);
        ChatHeader ack;
        fillHeader(ack, CHAT_ACK, sessionId, hdr->messageId);
        esp_now_send(remoteMac, reinterpret_cast<uint8_t*>(&ack), sizeof(ack));
    } else if (hdr->type == CHAT_ACK) {
        markDelivered(hdr->messageId);
    } else if (hdr->type == CHAT_BYE) {
        addHistory(false, "DISCONNECTED", 0, true);
        disconnectPeer(false);
    }
}

void sendMessage() {
    if (!connected || inputLen == 0) return;
    ChatMessagePacket packet = {};
    uint16_t id = nextMessageId++;
    fillHeader(packet.hdr, CHAT_MESSAGE, sessionId, id);
    packet.textLen = inputLen;
    memcpy(packet.text, input, inputLen);
    size_t packetLen = sizeof(ChatHeader) + 1 + inputLen;
    esp_now_send(remoteMac, reinterpret_cast<uint8_t*>(&packet), packetLen);
    addHistory(true, input, id, false);
    inputLen = 0;
    input[0] = 0;
}

void handleInput() {
    bool pressed = M5Cardputer.Keyboard.isPressed();
    if (!pressed) {
        keyLatched = false;
        return;
    }
    if (keyLatched) return;
    keyLatched = true;
    auto keys = M5Cardputer.Keyboard.keysState();

    if (!connected) {
        if (peerCount && M5Cardputer.Keyboard.isKeyPressed(';')) {
            selectedPeer = selectedPeer ? selectedPeer - 1 : peerCount - 1;
        } else if (peerCount && M5Cardputer.Keyboard.isKeyPressed('.')) {
            selectedPeer = (selectedPeer + 1) % peerCount;
        } else if (peerCount && keys.enter && !connecting) {
            memcpy(remoteMac, peers[selectedPeer].mac, 6);
            strncpy(remoteName, peers[selectedPeer].name, sizeof(remoteName) - 1);
            sessionId = static_cast<uint16_t>(esp_random() & 0xffffU);
            if (!sessionId) sessionId = 1;
            addPeer(remoteMac, false);
            sendNamed(remoteMac, CHAT_HELLO, sessionId);
            connecting = true;
            connectStarted = millis();
        }
        return;
    }

    if (keys.enter) {
        sendMessage();
        return;
    }
    if (keys.del) {
        if (inputLen) input[--inputLen] = 0;
        return;
    }
    for (char c : keys.word) {
        if (c >= 32 && c <= 126 && c != '`' && inputLen < MAX_TEXT) {
            input[inputLen++] = c;
            input[inputLen] = 0;
        }
    }
}
} // namespace

void PigChatMode::start() {
    if (running) return;
    NetworkRecon::pause();
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    delay(100);
    esp_wifi_set_channel(CHAT_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) return;
    static const uint8_t pmk[16] = {
        'P','I','G','C','H','A','T','-','P','M','K','-','2','0','2','6'
    };
    esp_now_set_pmk(pmk);
    esp_now_register_recv_cb(onChatRecv);
    esp_now_register_send_cb(onChatSent);
    addPeer(broadcastMac, false);
    peerCount = selectedPeer = 0;
    historyStart = historyCount = 0;
    inputLen = 0;
    input[0] = 0;
    disconnectPeer(false);
    rxRead = rxWrite = 0;
    keyLatched = true;
    lastDiscover = 0;
    running = true;
}

void PigChatMode::stop() {
    if (!running) return;
    disconnectPeer(true);
    running = false;
    esp_now_deinit();
    NetworkRecon::start();
}

void PigChatMode::update() {
    if (!running) return;
    while (rxRead != rxWrite) {
        RxSlot slot;
        taskENTER_CRITICAL(&rxMux);
        slot = rxQueue[rxRead];
        rxQueue[rxRead].used = false;
        rxRead = (rxRead + 1) % RX_QUEUE_SIZE;
        taskEXIT_CRITICAL(&rxMux);
        processPacket(slot);
    }
    uint32_t now = millis();
    if (!connected && !connecting && now - lastDiscover >= 750) {
        lastDiscover = now;
        sendNamed(broadcastMac, CHAT_DISCOVER);
    }
    if (connecting && now - connectStarted > 5000) {
        addHistory(false, "CONNECT TIMEOUT", 0, true);
        disconnectPeer(false);
    }
    handleInput();
}

void PigChatMode::draw(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    if (!connected) {
        canvas.drawString(connecting ? "PIGCHAT - CONNECTING" : "PIGCHAT - NEARBY PIGS", 4, 3);
        if (!peerCount) {
            canvas.drawString("SCANNING CH1...", 4, 25);
        } else {
            for (uint8_t i = 0; i < peerCount && i < 6; i++) {
                char line[40];
                snprintf(line, sizeof(line), "%c %-15s %02X:%02X:%02X", i == selectedPeer ? '>' : ' ',
                         peers[i].name, peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
                canvas.drawString(line, 4, 18 + i * 13);
            }
        }
        canvas.drawString("[;/.] SELECT  [ENTER] CHAT", 4, 102);
        return;
    }

    char header[40];
    snprintf(header, sizeof(header), "PIGCHAT > %s", remoteName[0] ? remoteName : "PIG");
    canvas.drawString(header, 4, 2);
    uint8_t visible = min<uint8_t>(historyCount, 5);
    uint8_t first = historyCount - visible;
    for (uint8_t i = 0; i < visible; i++) {
        const HistoryEntry& entry = history[(historyStart + first + i) % HISTORY_SIZE];
        char line[42];
        snprintf(line, sizeof(line), "%s %.32s%s", entry.mine ? "ME>" : "PG>", entry.text,
                 entry.mine ? (entry.delivered ? " +" : " ...") : "");
        canvas.drawString(line, 4, 16 + i * 14);
    }
    canvas.drawRect(2, 88, canvas.width() - 4, 18, COLOR_FG);
    char edit[42];
    snprintf(edit, sizeof(edit), "> %.34s", input);
    canvas.drawString(edit, 5, 92);
    canvas.drawString("ENTER SEND  DEL ERASE  ESC EXIT", 4, 108);
}
