// LeMidiReci — ESP32 BLE-MIDI receiver.
// Copyright (C) 2026 RichardSmetana
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "midi_ble.h"

// 0 = ESP32-WROOM-32, 1 = ESP32-C3.
// Leave this commented out to use the board selected in the Arduino IDE.
// #define BOARD_ESP32C3 0

#if defined(BOARD_ESP32C3)
#define IS_ESP32C3 (BOARD_ESP32C3)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define IS_ESP32C3 1
#else
#define IS_ESP32C3 0
#endif

// USB serial (not the hardware MIDI UART):
// 0 = no USB output, 1 = binary MIDI bytes only, 2 = debug text.
#define SERIAL_MODE 2

// 1 = use the OLED, 0 = display off. The sketch runs with no display fitted.
#define USE_DISPLAY 1

// The core defines LED_BUILTIN itself. Clear it so the line below takes effect.
#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif


// Hardware MIDI out at 31250 8N1. Independent of SERIAL_MODE.
// 1 = send remapped MIDI on the UART below. 0 = UART silent and pins untouched.
#define ENABLE_UART_MIDI 1
#define MIDI_BAUD 31250
// TX is MIDI out. RX is claimed by the UART but this sketch does not read MIDI in.
#if IS_ESP32C3
#define MIDI_UART_TX 7
#define MIDI_UART_RX 6
// Activity LED. Comment this out to leave the pin untouched.
// GPIO 8 is a strapping pin on the ESP32-C3. Enable it only on a board that wires an LED there.
#define LED_BUILTIN 8
// 1 = the LED lights when the pin is low. 0 = the LED lights when the pin is high.
#define LED_ACTIVE_LOW 1
#else
#define MIDI_UART_TX 17
#define MIDI_UART_RX 16
//#define LED_BUILTIN 8
//#define LED_ACTIVE_LOW 1
#endif

#if ENABLE_UART_MIDI
#if MIDI_UART_TX == MIDI_UART_RX
#error MIDI_UART_TX and MIDI_UART_RX must be different pins
#endif
#if MIDI_UART_TX == 4 || MIDI_UART_TX == 5 || MIDI_UART_RX == 4 || MIDI_UART_RX == 5
#error MIDI UART pins collide with the OLED I2C pins GPIO 4 and GPIO 5
#endif
#if defined(LED_BUILTIN)
#if MIDI_UART_TX == LED_BUILTIN || MIDI_UART_RX == LED_BUILTIN
#error MIDI UART pins collide with the activity LED
#endif
#endif
#if IS_ESP32C3
#if MIDI_UART_TX == 2 || MIDI_UART_TX == 8 || MIDI_UART_TX == 9 || MIDI_UART_TX == 18 || MIDI_UART_TX == 19 \
  || MIDI_UART_RX == 2 || MIDI_UART_RX == 8 || MIDI_UART_RX == 9 || MIDI_UART_RX == 18 || MIDI_UART_RX == 19
#error MIDI UART pins collide with USB or a strapping pin on the ESP32-C3
#endif
#define MIDI_UART Serial1
#else
#if MIDI_UART_TX == 0 || MIDI_UART_TX == 1 || MIDI_UART_TX == 3 || (MIDI_UART_TX >= 6 && MIDI_UART_TX <= 11) \
  || MIDI_UART_RX == 0 || MIDI_UART_RX == 1 || MIDI_UART_RX == 3 || (MIDI_UART_RX >= 6 && MIDI_UART_RX <= 11)
#error MIDI UART pins collide with boot, the USB-UART console, or flash on the ESP32-WROOM-32
#endif
#define MIDI_UART Serial2
#endif
#endif

#if defined(LED_BUILTIN)
#define MIDI_LED_PIN LED_BUILTIN
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif
#if LED_ACTIVE_LOW
#define MIDI_LED_ON LOW
#define MIDI_LED_OFF HIGH
#else
#define MIDI_LED_ON HIGH
#define MIDI_LED_OFF LOW
#endif
#endif

// Device -> MIDI channel 1..16.
// id is the BLE address (aa:bb:cc:dd:ee:ff) or the advertised name.
// An address match wins. The first match wins. With no match the channel stays 1.
static const DeviceChannel kDeviceChannels[] = {
  { "SMC-PAD Pocket", 2 },
  { "SMC-PAD", 3 },
};

#if USE_DISPLAY
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#include <NimBLEDevice.h>
#include <cstdio>
#include <cstring>

#if SERIAL_MODE > 0
#define SERIAL_BEGIN() Serial.begin(115200)
#else
#define SERIAL_BEGIN()
#endif

#if SERIAL_MODE == 2
#define DBG_PRINT(...) Serial.printf(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DBG_PRINT(...)
#define DBG_PRINTLN(...)
#endif

#if USE_DISPLAY
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
// Same wiring on both modules: SDA GPIO 5, SCL GPIO 4.
#define OLED_SDA 5
#define OLED_SCL 4
#endif

// Bluetooth MIDI (MMA / Apple)
static const NimBLEUUID midiServiceUuid("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
static const NimBLEUUID midiCharUuid("7772e5db-3868-4112-a1a9-f2669d106bf3");

// The ESP32 controller supports 3 connections. 520 = 0x208 = supervision timeout.
static const uint8_t MAX_LINKS = 3;
static const int BLE_TIMEOUT = 520;
// Short pause before the next scan so a drop does not immediately load the radio.
static const uint32_t RECONNECT_DELAY_MS = 500;
// After a new central link, wait before scanning for another device.
static const uint32_t SCAN_SETTLE_MS = 1000;

#if USE_DISPLAY
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool oledOk = false;
#endif

static NimBLEServer* pServer = nullptr;
static NimBLEAddress pendingAddress;
static volatile bool pendingReady = false;
static volatile bool needScan = false;
static volatile bool advDirty = true;
static volatile uint32_t nextScanAt = 0;
static volatile int lastReason = 0;

struct PeerParser {
  char peer[18];
  bool used;
  MidiParser parser;
};

struct LinkView {
  char peer[18];
  char label[12];
  uint8_t channel;
  bool used;
};

struct ViewEvent {
  uint8_t raw[3];
  uint8_t out[3];
  uint8_t length;
  uint16_t sysexLen;
  uint8_t originalChannel;
  uint8_t targetChannel;
};

struct DebugEvent {
  char peer[18];
  uint8_t raw[8];
  uint8_t out[8];
  uint8_t stored;
  uint16_t length;
  uint8_t originalChannel;
  uint8_t targetChannel;
  uint16_t timestamp;
  bool uartDropped;
  bool usbDropped;
};

struct KnownPeer {
  char address[18];
  char name[32];
};

#if defined(LED_BUILTIN)
static volatile uint32_t midiLedUntil = 0;

void midiLedPulse() {
  digitalWrite(MIDI_LED_PIN, MIDI_LED_ON);
  midiLedUntil = millis() + 30;
}

void midiLedPoll() {
  if (midiLedUntil != 0 && (int32_t)(millis() - midiLedUntil) >= 0) {
    digitalWrite(MIDI_LED_PIN, MIDI_LED_OFF);
    midiLedUntil = 0;
  }
}
#else
void midiLedPulse() {}
void midiLedPoll() {}
#endif

static uint8_t uartStorage[256];
static uint8_t usbStorage[256];
static MidiByteQueue uartQueue;
static MidiByteQueue usbQueue;

static uint16_t statMessages = 0;
static uint16_t statErrors = 0;
static uint16_t statOverflows = 0;
static uint16_t statWraps = 0;
static uint16_t statClock = 0;
static uint16_t statSense = 0;

static PeerParser peerParsers[MAX_LINKS];
static LinkView linkView[MAX_LINKS];
static ViewEvent viewLog[6];
static volatile uint8_t viewWrite = 0;
static volatile uint8_t viewCount = 0;
static volatile bool displayDirty = false;
static DebugEvent debugQueue[12];
static volatile uint8_t debugHead = 0;
static volatile uint8_t debugTail = 0;
static uint16_t debugDrops = 0;
static KnownPeer knownPeers[8];

uint8_t clientCount() {
  return pServer == nullptr ? 0 : pServer->getConnectedCount();
}

uint8_t outgoingCount() {
  return (uint8_t)NimBLEDevice::getConnectedClients().size();
}

uint8_t usedLinks() {
  return clientCount() + outgoingCount();
}

bool isLinked(const NimBLEAddress& address) {
  NimBLEClient* client = NimBLEDevice::getClientByPeerAddress(address);
  return client != nullptr && client->isConnected();
}

void scheduleScan(uint32_t delayMs) {
  nextScanAt = millis() + delayMs;
  needScan = false;
}

void copyText(char* dest, size_t size, const char* src) {
  if (size == 0) {
    return;
  }
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, size - 1);
  dest[size - 1] = '\0';
}

void rememberPeer(const char* address, const char* name) {
  if (address == nullptr || address[0] == '\0') {
    return;
  }
  for (size_t i = 0; i < sizeof(knownPeers) / sizeof(knownPeers[0]); i++) {
    if (midiSameText(knownPeers[i].address, address)) {
      if (name != nullptr && name[0] != '\0') {
        copyText(knownPeers[i].name, sizeof(knownPeers[i].name), name);
      }
      return;
    }
  }
  for (size_t i = 0; i < sizeof(knownPeers) / sizeof(knownPeers[0]); i++) {
    if (knownPeers[i].address[0] == '\0') {
      copyText(knownPeers[i].address, sizeof(knownPeers[i].address), address);
      copyText(knownPeers[i].name, sizeof(knownPeers[i].name), name);
      return;
    }
  }
}

const char* nameFor(const char* address) {
  if (address == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < sizeof(knownPeers) / sizeof(knownPeers[0]); i++) {
    if (midiSameText(knownPeers[i].address, address) && knownPeers[i].name[0] != '\0') {
      return knownPeers[i].name;
    }
  }
  return nullptr;
}

uint8_t channelFor(const char* address) {
  return midiChannelFor(address, nameFor(address), kDeviceChannels,
                        sizeof(kDeviceChannels) / sizeof(kDeviceChannels[0]));
}

void makeLabel(char* dest, size_t size, const char* address) {
  const char* name = nameFor(address);
  if (name != nullptr && name[0] != '\0') {
    copyText(dest, size, name);
    return;
  }
  if (address == nullptr || address[0] == '\0') {
    copyText(dest, size, "?");
    return;
  }
  size_t len = strlen(address);
  copyText(dest, size, len > 8 ? address + len - 8 : address);
}

void linkUp(const char* address) {
  if (address == nullptr || address[0] == '\0') {
    return;
  }
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (linkView[i].used && midiSameText(linkView[i].peer, address)) {
      linkView[i].channel = channelFor(address);
      makeLabel(linkView[i].label, sizeof(linkView[i].label), address);
      displayDirty = true;
      return;
    }
  }
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (!linkView[i].used) {
      copyText(linkView[i].peer, sizeof(linkView[i].peer), address);
      linkView[i].channel = channelFor(address);
      makeLabel(linkView[i].label, sizeof(linkView[i].label), address);
      linkView[i].used = true;
      displayDirty = true;
      return;
    }
  }
}

void linkDown(const char* address) {
  if (address == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (linkView[i].used && midiSameText(linkView[i].peer, address)) {
      linkView[i].used = false;
      displayDirty = true;
    }
  }
}

PeerParser* parserFor(const char* peer) {
  const char* name = (peer != nullptr && peer[0] != '\0') ? peer : "?";
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (peerParsers[i].used && midiSameText(peerParsers[i].peer, name)) {
      return &peerParsers[i];
    }
  }
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (!peerParsers[i].used) {
      midiParserInit(&peerParsers[i].parser);
      copyText(peerParsers[i].peer, sizeof(peerParsers[i].peer), name);
      peerParsers[i].used = true;
      return &peerParsers[i];
    }
  }
  return nullptr;
}

void releaseParser(const char* peer) {
  if (peer == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < MAX_LINKS; i++) {
    if (peerParsers[i].used && midiSameText(peerParsers[i].peer, peer)) {
      peerParsers[i].used = false;
      midiParserInit(&peerParsers[i].parser);
    }
  }
}

void addStatDelta(const MidiParseStats* before, const MidiParseStats* after) {
  if (after->messages >= before->messages) {
    statMessages = (uint16_t)(statMessages + (after->messages - before->messages));
  }
  if (after->errors >= before->errors) {
    statErrors = (uint16_t)(statErrors + (after->errors - before->errors));
  }
  if (after->sysexOverflows >= before->sysexOverflows) {
    statOverflows = (uint16_t)(statOverflows + (after->sysexOverflows - before->sysexOverflows));
  }
  if (after->timestampWraps >= before->timestampWraps) {
    statWraps = (uint16_t)(statWraps + (after->timestampWraps - before->timestampWraps));
  }
}

void pushMidiView(const MidiMessage* message) {
#if !USE_DISPLAY
  (void)message;
#else
  if (message == nullptr || message->length == 0) {
    return;
  }
  ViewEvent* slot = &viewLog[viewWrite % 6];
  memset(slot, 0, sizeof(*slot));
  slot->originalChannel = message->originalChannel;
  slot->targetChannel = message->targetChannel;
  if (message->data[0] == 0xF0 || message->length > 3) {
    slot->length = 0;
    slot->sysexLen = message->length;
  } else {
    slot->length = (uint8_t)message->length;
    memcpy(slot->out, message->data, message->length);
    memcpy(slot->raw, message->data, message->length);
    if (message->originalChannel != 0) {
      slot->raw[0] = (uint8_t)((message->data[0] & 0xF0) | (message->originalChannel - 1));
    }
  }
  __sync_synchronize();
  viewWrite = (uint8_t)((viewWrite + 1) % 6);
  if (viewCount < 6) {
    viewCount++;
  }
  displayDirty = true;
#endif
}

void enqueueDebug(const PeerParser* peer, const MidiMessage* message, bool uartDropped, bool usbDropped) {
#if SERIAL_MODE != 2
  (void)peer;
  (void)message;
  (void)uartDropped;
  (void)usbDropped;
#else
  if (message == nullptr) {
    return;
  }
  uint8_t next = (uint8_t)((debugHead + 1) % (sizeof(debugQueue) / sizeof(debugQueue[0])));
  if (next == debugTail) {
    if (debugDrops < 0xFFFF) {
      debugDrops++;
    }
    return;
  }
  DebugEvent& event = debugQueue[debugHead];
  memset(&event, 0, sizeof(event));
  copyText(event.peer, sizeof(event.peer), peer != nullptr ? peer->peer : "?");
  event.length = message->length;
  event.stored = message->length > sizeof(event.out) ? sizeof(event.out) : (uint8_t)message->length;
  memcpy(event.out, message->data, event.stored);
  memcpy(event.raw, message->data, event.stored);
  event.originalChannel = message->originalChannel;
  event.targetChannel = message->targetChannel;
  event.timestamp = message->timestamp;
  event.uartDropped = uartDropped;
  event.usbDropped = usbDropped;
  if (message->originalChannel != 0 && event.stored > 0) {
    event.raw[0] = (uint8_t)((message->data[0] & 0xF0) | (message->originalChannel - 1));
  }
  __sync_synchronize();
  debugHead = next;
#endif
}

void onMidiMessage(const MidiMessage* message, void* context) {
  if (message == nullptr || message->length == 0) {
    return;
  }
  MidiSinkMask mask;
  mask.uart = ENABLE_UART_MIDI;
  mask.usbBinary = SERIAL_MODE == 1;
  bool uartSent = false;
  bool usbSent = false;
  midiFanout(message->data, message->length, mask, &uartQueue, &usbQueue, &uartSent, &usbSent);

  bool clockOrSense = message->length == 1 && (message->data[0] == 0xF8 || message->data[0] == 0xFE);
  if (clockOrSense) {
    if (message->data[0] == 0xF8) {
      if (statClock < 0xFFFF) {
        statClock++;
      }
    } else if (statSense < 0xFFFF) {
      statSense++;
    }
    return;
  }

  midiLedPulse();
  pushMidiView(message);
  enqueueDebug((const PeerParser*)context, message, mask.uart && !uartSent, mask.usbBinary && !usbSent);
}

void acceptMidi(const char* peer, const uint8_t* data, size_t length) {
  PeerParser* slot = parserFor(peer);
  if (slot == nullptr || data == nullptr || length == 0) {
    if (statErrors < 0xFFFF) {
      statErrors++;
    }
    return;
  }
  MidiParseStats before = slot->parser.stats;
  midiParsePacket(&slot->parser, data, length, channelFor(slot->peer), onMidiMessage, slot);
  addStatDelta(&before, &slot->parser.stats);
}

void appendHex(char* dest, size_t size, const uint8_t* bytes, uint8_t length) {
  size_t pos = strlen(dest);
  for (uint8_t i = 0; i < length && pos + 4 < size; i++) {
    int wrote = snprintf(dest + pos, size - pos, i == 0 ? "%02X" : " %02X", bytes[i]);
    if (wrote < 0) {
      break;
    }
    pos += (size_t)wrote;
  }
}

void drainOutputs() {
#if ENABLE_UART_MIDI
  uint8_t buf[64];
  while (midiQueueCount(&uartQueue) > 0) {
    int space = MIDI_UART.availableForWrite();
    if (space <= 0) {
      break;
    }
    if (space > (int)sizeof(buf)) {
      space = (int)sizeof(buf);
    }
    uint16_t count = midiQueuePop(&uartQueue, buf, (uint16_t)space);
    if (count == 0) {
      break;
    }
    MIDI_UART.write(buf, count);
  }
#endif

#if SERIAL_MODE == 1
  uint8_t usbBuf[64];
  while (midiQueueCount(&usbQueue) > 0) {
    int space = Serial.availableForWrite();
    if (space <= 0) {
      break;
    }
    if (space > (int)sizeof(usbBuf)) {
      space = (int)sizeof(usbBuf);
    }
    uint16_t count = midiQueuePop(&usbQueue, usbBuf, (uint16_t)space);
    if (count == 0) {
      break;
    }
    Serial.write(usbBuf, count);
  }
#elif SERIAL_MODE == 2
  uint8_t printed = 0;
  while (debugTail != debugHead && printed < 4) {
    __sync_synchronize();
    DebugEvent event = debugQueue[debugTail];
    debugTail = (uint8_t)((debugTail + 1) % (sizeof(debugQueue) / sizeof(debugQueue[0])));
    printed++;
    if (event.originalChannel != 0) {
      char raw[28];
      char out[28];
      raw[0] = '\0';
      out[0] = '\0';
      appendHex(raw, sizeof(raw), event.raw, event.stored);
      appendHex(out, sizeof(out), event.out, event.stored);
      Serial.printf("IN %s CH%u->CH%u %s -> %s t%u", event.peer, event.originalChannel, event.targetChannel, raw,
                    out, event.timestamp);
    } else if (event.stored > 0 && event.raw[0] == 0xF0) {
      Serial.printf("IN %s SYSEX %u t%u", event.peer, event.length, event.timestamp);
    } else if (event.length == 1 && event.stored > 0 && event.out[0] >= 0xF8) {
      Serial.printf("IN %s RT %02X t%u", event.peer, event.out[0], event.timestamp);
    } else {
      char out[28];
      out[0] = '\0';
      appendHex(out, sizeof(out), event.out, event.stored);
      Serial.printf("IN %s %s t%u", event.peer, out, event.timestamp);
    }
    if (event.length > event.stored && event.raw[0] != 0xF0) {
      Serial.printf(" +%u", (unsigned)(event.length - event.stored));
    }
    if (event.uartDropped) {
      Serial.print(" UART-DROP");
    }
    if (event.usbDropped) {
      Serial.print(" USB-DROP");
    }
    Serial.println();
  }
#endif
}

void formatViewLine(char* line, size_t size, const ViewEvent* event) {
  if (event->length == 0) {
    snprintf(line, size, "SYX %u", event->sysexLen);
    return;
  }
  char raw[16];
  char out[16];
  raw[0] = '\0';
  out[0] = '\0';
  appendHex(raw, sizeof(raw), event->raw, event->length);
  appendHex(out, sizeof(out), event->out, event->length);
  if (event->originalChannel != 0 && event->originalChannel != event->targetChannel) {
    snprintf(line, size, "%s>%s", raw, out);
  } else {
    snprintf(line, size, "%s", out);
  }
}

void drawScreen() {
#if USE_DISPLAY
  if (!oledOk) {
    return;
  }

  char lines[8][22];
  memset(lines, 0, sizeof(lines));
  int reason = lastReason;
  if (reason != 0) {
    snprintf(lines[0], sizeof(lines[0]), "BLE %u/%u r%d", usedLinks(), MAX_LINKS, reason);
  } else {
    snprintf(lines[0], sizeof(lines[0]), "BLE %u/%u", usedLinks(), MAX_LINKS);
  }

  uint8_t row = 1;
  for (uint8_t i = 0; i < MAX_LINKS && row < 4; i++) {
    if (!linkView[i].used) {
      continue;
    }
    snprintf(lines[row], sizeof(lines[row]), "%-10.10s CH%02u", linkView[i].label, linkView[i].channel);
    row++;
  }

  uint8_t available = (uint8_t)(8 - row);
  uint8_t count = viewCount;
  uint8_t show = count > available ? available : count;
  uint8_t write = viewWrite;
  for (uint8_t n = 0; n < show; n++) {
    uint8_t age = (uint8_t)(show - 1 - n);
    uint8_t idx = (uint8_t)((write + 6 - 1 - age) % 6);
    formatViewLine(lines[row + n], sizeof(lines[0]), &viewLog[idx]);
  }

  static char shown[8][22];
  if (memcmp(shown, lines, sizeof(lines)) == 0) {
    return;
  }
  memcpy(shown, lines, sizeof(lines));

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);
  for (uint8_t line = 0; line < 8; line++) {
    if (lines[line][0] == '\0') {
      continue;
    }
    display.setCursor(0, line * 8);
    display.print(lines[line]);
  }
  display.display();
#endif
}

void refreshAdvertising() {
  if (pServer == nullptr) {
    return;
  }

  // Advertising beside an open central MIDI link steals radio time,
  // and the peripheral can disconnect with reason 520.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  bool want = outgoingCount() == 0 && !pendingReady && clientCount() < MAX_LINKS && usedLinks() < MAX_LINKS;
  if (want) {
    if (!advertising->isAdvertising() && advertising->start()) {
      DBG_PRINT("Advertising on, incoming %u/%u\n", clientCount(), MAX_LINKS);
    }
  } else if (advertising->isAdvertising()) {
    advertising->stop();
  }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* client) override {
    advDirty = true;
    lastReason = 0;
    std::string addr = client->getPeerAddress().toString();
    linkUp(addr.c_str());
    DBG_PRINT("MIDI %u/%u connected: %s CH%u\n", outgoingCount(), MAX_LINKS, addr.c_str(), channelFor(addr.c_str()));
  }

  void onConnectFail(NimBLEClient* client, int reason) override {
    DBG_PRINT("MIDI connect failed, reason %d, %s\n", reason, client->getPeerAddress().toString().c_str());
    lastReason = reason;
    scheduleScan(RECONNECT_DELAY_MS);
    advDirty = true;
  }

  void onDisconnect(NimBLEClient* client, int reason) override {
    std::string addr = client->getPeerAddress().toString();
    DBG_PRINT("MIDI disconnected: %s, reason %d%s\n", addr.c_str(), reason,
              reason == BLE_TIMEOUT ? " (supervision timeout)" : "");
    lastReason = reason;
    releaseParser(addr.c_str());
    linkDown(addr.c_str());
    scheduleScan(RECONNECT_DELAY_MS);
    advDirty = true;
  }
} clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (pendingReady || usedLinks() >= MAX_LINKS) {
      return;
    }
    if (advertisedDevice->getAddress() == NimBLEDevice::getAddress()) {
      return;
    }
    if (!advertisedDevice->isConnectable()) {
      return;
    }
    if (!advertisedDevice->isAdvertisingService(midiServiceUuid)) {
      return;
    }
    if (isLinked(advertisedDevice->getAddress())) {
      return;
    }

    std::string foundAddr = advertisedDevice->getAddress().toString();
    std::string foundName = advertisedDevice->getName();
    rememberPeer(foundAddr.c_str(), foundName.c_str());
    DBG_PRINT("MIDI found: %s %s RSSI %d\n", foundName.c_str(), foundAddr.c_str(), advertisedDevice->getRSSI());
    pendingAddress = advertisedDevice->getAddress();
    __sync_synchronize();
    pendingReady = true;
    NimBLEDevice::getScan()->stop();
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
    if (!pendingReady && nextScanAt == 0 && usedLinks() < MAX_LINKS) {
      needScan = true;
    }
  }
} scanCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    std::string addr = connInfo.getAddress().toString();
    DBG_PRINT("Client %u/%u connected: %s\n", server->getConnectedCount(), MAX_LINKS, addr.c_str());
    lastReason = 0;
    linkUp(addr.c_str());
    server->updateConnParams(connInfo.getConnHandle(), 24, 40, 0, 600);
    advDirty = true;
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    std::string addr = connInfo.getAddress().toString();
    DBG_PRINT("Client disconnected: %s, reason %d, %u/%u remain\n", addr.c_str(), reason, server->getConnectedCount(),
              MAX_LINKS);
    lastReason = reason;
    releaseParser(addr.c_str());
    linkDown(addr.c_str());
    scheduleScan(RECONNECT_DELAY_MS);
    advDirty = true;
  }
} serverCallbacks;

class MidiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = characteristic->getValue();
    std::string peer = connInfo.getAddress().toString();
    acceptMidi(peer.c_str(), value.data(), value.size());
  }
} midiCallbacks;

void notifyCB(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  (void)isNotify;
  std::string peer = characteristic->getClient()->getPeerAddress().toString();
  acceptMidi(peer.c_str(), data, length);
}

bool connectToMidi(const NimBLEAddress& address) {
  if (isLinked(address) || usedLinks() >= MAX_LINKS) {
    return false;
  }

  NimBLEClient* client = NimBLEDevice::getClientByPeerAddress(address);
  if (client == nullptr) {
    client = NimBLEDevice::getDisconnectedClient();
  }
  if (client == nullptr) {
    client = NimBLEDevice::createClient();
    if (client == nullptr) {
      DBG_PRINTLN("No free BLE client");
      return false;
    }
  }

  client->setClientCallbacks(&clientCallbacks, false);
  client->setConnectTimeout(8000);
  // Devices that are already connected need radio time during this connect.
  uint16_t scanInterval = outgoingCount() > 0 ? 80 : 16;
  client->setConnectionParams(24, 40, 0, 600, scanInterval, 16);

  if (!client->isConnected() && !client->connect(address)) {
    DBG_PRINT("Connection rejected: %s\n", address.toString().c_str());
    return false;
  }

  NimBLERemoteService* service = client->getService(midiServiceUuid);
  if (service == nullptr) {
    DBG_PRINTLN("MIDI service missing");
    client->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* characteristic = service->getCharacteristic(midiCharUuid);
  if (characteristic == nullptr) {
    DBG_PRINTLN("MIDI characteristic missing");
    client->disconnect();
    return false;
  }

  bool notifications = characteristic->canNotify() || !characteristic->canIndicate();
  bool subscribed = characteristic->subscribe(notifications, notifyCB, true);
  if (!subscribed) {
    subscribed = characteristic->subscribe(notifications, notifyCB, false);
  }
  if (!subscribed) {
    DBG_PRINTLN("Notify failed");
    client->disconnect();
    return false;
  }

  DBG_PRINT("MIDI notify on: %s\n", address.toString().c_str());
  return true;
}

void startScan() {
  if (pendingReady || usedLinks() >= MAX_LINKS) {
    return;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    return;
  }

  // A wide scan window while a link is up was enough to cause supervision timeouts.
  if (outgoingCount() > 0 || clientCount() > 0) {
    scan->setInterval(240);
    scan->setWindow(40);
  } else {
    scan->setInterval(100);
    scan->setWindow(80);
  }

  if (!scan->start(0, false, true)) {
    scheduleScan(RECONNECT_DELAY_MS);
    return;
  }
  DBG_PRINT("Scanning for MIDI devices %u/%u\n", outgoingCount(), MAX_LINKS);
}

void printStats() {
#if SERIAL_MODE == 2
  static uint32_t lastPrint = 0;
  static uint16_t seenMessages = 0;
  static uint16_t seenErrors = 0;
  static uint16_t seenOverflows = 0;
  static uint16_t seenWraps = 0;
  static uint16_t seenClock = 0;
  static uint16_t seenSense = 0;
  static uint16_t seenUartDrops = 0;
  static uint16_t seenUsbDrops = 0;
  static uint16_t seenDebugDrops = 0;
  uint32_t now = millis();
  if ((int32_t)(now - lastPrint) < 5000) {
    return;
  }
  lastPrint = now;
  bool changed = statMessages != seenMessages || statErrors != seenErrors || statOverflows != seenOverflows || statWraps != seenWraps || statClock != seenClock || statSense != seenSense || uartQueue.drops != seenUartDrops || usbQueue.drops != seenUsbDrops || debugDrops != seenDebugDrops;
  if (!changed) {
    return;
  }
  seenMessages = statMessages;
  seenErrors = statErrors;
  seenOverflows = statOverflows;
  seenWraps = statWraps;
  seenClock = statClock;
  seenSense = statSense;
  seenUartDrops = uartQueue.drops;
  seenUsbDrops = usbQueue.drops;
  seenDebugDrops = debugDrops;
  Serial.printf("stats msg=%u err=%u syxOv=%u wrap=%u clock=%u sense=%u uartDrop=%u usbDrop=%u dbgDrop=%u\n",
                statMessages, statErrors, statOverflows, statWraps, statClock, statSense, uartQueue.drops,
                usbQueue.drops, debugDrops);
#endif
}

void setup() {
  midiQueueInit(&uartQueue, uartStorage, sizeof(uartStorage));
  midiQueueInit(&usbQueue, usbStorage, sizeof(usbStorage));

#if defined(LED_BUILTIN)
  pinMode(MIDI_LED_PIN, OUTPUT);
  digitalWrite(MIDI_LED_PIN, MIDI_LED_OFF);
#endif
  SERIAL_BEGIN();
#if IS_ESP32C3 && SERIAL_MODE > 0
  // The ESP32-C3 uses USB serial. Wait briefly until the port is open.
  uint32_t usbWait = millis();
  while (!Serial && millis() - usbWait < 1500) {
    delay(10);
  }
#endif

#if ENABLE_UART_MIDI
  MIDI_UART.begin(MIDI_BAUD, SERIAL_8N1, MIDI_UART_RX, MIDI_UART_TX);
  DBG_PRINT("UART MIDI TX GPIO %d at %u\n", MIDI_UART_TX, MIDI_BAUD);
#endif

#if USE_DISPLAY
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
  if (!oledOk) {
    DBG_PRINTLN("OLED not found");
  }
  drawScreen();
#endif

  NimBLEDevice::init("LeMidiReci");
  NimBLEDevice::setPower(9);
  NimBLEDevice::setMTU(128);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);
  pServer->advertiseOnDisconnect(false);

  NimBLEService* midiService = pServer->createService(midiServiceUuid);
  NimBLECharacteristic* midiChar = midiService->createCharacteristic(
    midiCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  midiChar->setValue((uint8_t)0x80);
  midiChar->setCallbacks(&midiCallbacks);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->setMinInterval(320);
  advertising->setMaxInterval(480);
  advertising->setName("LeMidiReci");
  advertising->addServiceUUID(midiServiceUuid);
  if (!advertising->start()) {
    DBG_PRINTLN("Advertising failed");
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  startScan();
}

void loop() {
  midiLedPoll();
  drainOutputs();

  bool justConnected = false;
  if (pendingReady) {
    __sync_synchronize();
    NimBLEAddress address = pendingAddress;
    pendingReady = false;
    if (NimBLEDevice::getAdvertising()->isAdvertising()) {
      NimBLEDevice::getAdvertising()->stop();
    }
    if (connectToMidi(address) && usedLinks() < MAX_LINKS) {
      scheduleScan(SCAN_SETTLE_MS);
      justConnected = true;
    } else if (usedLinks() < MAX_LINKS) {
      scheduleScan(RECONNECT_DELAY_MS);
    } else {
      needScan = false;
      nextScanAt = 0;
    }
    advDirty = true;
  }

  uint32_t now = millis();
  if (!justConnected && !pendingReady && (needScan || (nextScanAt != 0 && (int32_t)(now - nextScanAt) >= 0))) {
    needScan = false;
    nextScanAt = 0;
    startScan();
  }

  if (advDirty) {
    advDirty = false;
    refreshAdvertising();
  }

#if USE_DISPLAY
  static uint8_t shownLinks = 0xFF;
  static uint32_t lastDraw = 0;
  uint8_t links = usedLinks();
  bool linksChanged = links != shownLinks;
  if ((displayDirty || linksChanged) && (linksChanged || (int32_t)(now - lastDraw) >= 80)) {
    displayDirty = false;
    shownLinks = links;
    lastDraw = now;
    drawScreen();
  }
#endif

  printStats();
  delay(20);
}
