// drone_detect.cpp — see drone_detect.h. Original code for M5PORKCHOP (MIT).
#include "drone_detect.h"
#include <string.h>

namespace dronedet {
namespace {
constexpr uint8_t kUuidLo = 0xFA;   // 0xFFFA little-endian on air
constexpr uint8_t kUuidHi = 0xFF;
constexpr uint8_t kAppCode = 0x0D;  // ASTM Remote ID / OpenDroneID
constexpr uint8_t kMsgBasicId = 0x0;
}

DroneHit droneInspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                            uint8_t advLen, int8_t rssi) {
    DroneHit h;
    if (!addr || !adv) return h;

    uint8_t i = 0;
    while (i + 1 < advLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || (uint16_t)(i + 1 + fieldLen) > advLen) break;
        uint8_t type = adv[i + 1];
        const uint8_t* data = &adv[i + 2];
        uint8_t dataLen = fieldLen - 1;

        // 16-bit Service Data: [uuid LE][app code][counter][25-byte message]
        if (type == 0x16 && dataLen >= 4 &&
            data[0] == kUuidLo && data[1] == kUuidHi && data[2] == kAppCode) {
            h.isDrone = true;
            // message begins at data[4] (after uuid[2], appcode, counter).
            const uint8_t* msg = data + 4;
            uint8_t msgLen = (dataLen > 4) ? (dataLen - 4) : 0;
            if (msgLen >= 22) {
                uint8_t msgType = (msg[0] >> 4) & 0x0F;
                if (msgType == kMsgBasicId) {
                    // UAS ID is 20 bytes at msg[2..21].
                    uint8_t n = 20;
                    for (uint8_t k = 0; k < n; ++k) {
                        char c = (char)msg[2 + k];
                        h.idText[k] = (c >= 32 && c < 127) ? c : '\0';
                        if (h.idText[k] == '\0') break;
                    }
                    h.idText[20] = '\0';
                }
            }
            break;
        }
        i += 1 + fieldLen;
    }

    if (h.isDrone) {
        h.rssi = rssi;
        memcpy(h.addr, addr, 6);
    }
    return h;
}

} // namespace dronedet
