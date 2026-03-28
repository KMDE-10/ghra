/*
 * Laser M01 Continuous Distance Output
 *
 * Wiring:
 *   Yellow (TXD) → GPIO33
 *   Green  (RXD) ← GPIO32
 *   Red    (VIN) → 3V3
 *   Black  (GND) → GND
 */

#include <SoftwareSerial.h>

SoftwareSerial LZR(33, 32);

// Fast continuous measurement mode
const uint8_t CMD_FAST[]       = {0xAA,0x00,0x00,0x22,0x00,0x01,0x00,0x00,0x23};
const uint8_t CMD_CONTINUOUS[] = {0xAA,0x00,0x00,0x21,0x00,0x01,0x00,0x00,0x22};

bool checksumOK(const uint8_t* f, int n) {
    if (n < 3) return false;
    uint32_t s = 0;
    for (int i = 1; i < n - 1; i++) s += f[i];
    return ((uint8_t)s) == f[n - 1];
}

uint32_t decodeBCD(const uint8_t* b) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = v * 100 + ((b[i] >> 4) & 0x0F) * 10 + (b[i] & 0x0F);
    }
    return v;
}

void setup() {
    Serial.begin(115200);
    LZR.begin(9600);
    delay(500);
    LZR.write(CMD_CONTINUOUS, sizeof(CMD_CONTINUOUS));
    LZR.flush();
}

uint8_t buf[16];
int pos = 0;

void loop() {
    while (LZR.available()) {
        uint8_t x = LZR.read();
        if (pos == 0 && x != 0xAA) continue;
        buf[pos++] = x;
        if (pos == 13) {
            if (buf[4] == 0x00 && buf[5] == 0x04 && checksumOK(buf, 13)) {
                float m = decodeBCD(&buf[6]) / 1000.0f;
                Serial.printf("%.0f\n", m * 1000);
            }
            pos = 0;
        }
        if (pos >= 13) pos = 0;
    }
}
