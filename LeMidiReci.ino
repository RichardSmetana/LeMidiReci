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

// 0 = no output, 1 = binary MIDI commands only, 2 = debug text and hex dumps
#define SERIAL_MODE 2

// 1 = use the OLED, 0 = display off
#define USE_DISPLAY 0

// The core defines LED_BUILTIN itself. Clear it so the line below takes effect.
#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif
// LED on GPIO 8 flashes briefly on every MIDI command. Comment this out to disable it.
#define LED_BUILTIN 8

#if defined(LED_BUILTIN)
#define MIDI_LED_PIN LED_BUILTIN
#define MIDI_LED_ON LOW
#define MIDI_LED_OFF HIGH
#endif

// Device -> MIDI channel 1..16.
// id is the BLE address (aa:bb:cc:dd:ee:ff) or the advertised name.
// The first match wins. With no match the channel stays 1.
struct DeviceChannel {
  const char* id;
  uint8_t channel;
};

static const DeviceChannel kDeviceChannels[] = {
  { "SMC-PAD Pocket", 2 },
  { "SMC-PAD",        3 },
};

#if USE_DISPLAY
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#include <NimBLEDevice.h>
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

#if USE_DISPLAY
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool oledOk = false;
struct MidiLine {
  char text[12];
};
static MidiLine midiLog[8];
static volatile uint8_t midiLogWrite = 0;
static volatile uint8_t midiLogCount = 0;
static volatile bool displayDirty = false;
#endif

static NimBLEServer* pServer = nullptr;
static NimBLEAddress pendingAddress;
static volatile bool pendingReady = false;
static volatile bool needScan = false;
static volatile bool advDirty = true;
static volatile uint32_t nextScanAt = 0;
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

#if SERIAL_MODE == 1
struct MidiCmd {
  uint8_t data[3];
  uint8_t len;
};

static MidiCmd midiCmdQueue[16];
static volatile uint8_t midiCmdHead = 0;
static volatile uint8_t midiCmdTail = 0;
#elif SERIAL_MODE == 2
struct RxPacket {
  char peer[18];
  uint8_t data[48];
  uint8_t len;
  uint8_t channel;
};

static RxPacket rxQueue[8];
static volatile uint8_t rxHead = 0;
static volatile uint8_t rxTail = 0;
#endif

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

struct KnownPeer {
  char address[18];
  char name[32];
};

static KnownPeer knownPeers[8];

bool sameText(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca + 32);
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb + 32);
    }
    if (ca != cb) {
      return false;
    }
    a++;
    b++;
  }
  return *a == *b;
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
    if (sameText(knownPeers[i].address, address)) {
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
    if (sameText(knownPeers[i].address, address) && knownPeers[i].name[0] != '\0') {
      return knownPeers[i].name;
    }
  }
  return nullptr;
}

uint8_t channelFor(const char* address) {
  const char* name = nameFor(address);
  const size_t count = sizeof(kDeviceChannels) / sizeof(kDeviceChannels[0]);
  for (size_t i = 0; i < count; i++) {
    if (sameText(kDeviceChannels[i].id, address)) {
      uint8_t channel = kDeviceChannels[i].channel;
      return channel >= 1 && channel <= 16 ? channel : 1;
    }
  }
  if (name != nullptr) {
    for (size_t i = 0; i < count; i++) {
      if (sameText(kDeviceChannels[i].id, name)) {
        uint8_t channel = kDeviceChannels[i].channel;
        return channel >= 1 && channel <= 16 ? channel : 1;
      }
    }
  }
  return 1;
}

int8_t midiDataLength(uint8_t status) {
  if (status >= 0xF8) {
    return 0;
  }
  if (status >= 0xF0) {
    switch (status) {
      case 0xF1:
      case 0xF3:
        return 1;
      case 0xF2:
        return 2;
      case 0xF0:
        return -1;
      default:
        return 0;
    }
  }
  switch (status & 0xF0) {
    case 0xC0:
    case 0xD0:
      return 1;
    default:
      return 2;
  }
}

void enqueueMidiCmd(const uint8_t* bytes, uint8_t len) {
#if SERIAL_MODE == 1
  if (bytes == nullptr || len == 0) {
    return;
  }
  if (len > 3) {
    bytes += len - 3;
    len = 3;
  }
  uint8_t next = (uint8_t)((midiCmdHead + 1) % (sizeof(midiCmdQueue) / sizeof(midiCmdQueue[0])));
  if (next == midiCmdTail) {
    return;
  }
  memcpy(midiCmdQueue[midiCmdHead].data, bytes, len);
  midiCmdQueue[midiCmdHead].len = len;
  __sync_synchronize();
  midiCmdHead = next;
#else
  (void)bytes;
  (void)len;
#endif
}

void enqueueDebugRx(const char* peer, const uint8_t* data, size_t length, uint8_t channel) {
#if SERIAL_MODE == 2
  if (data == nullptr || length == 0) {
    return;
  }
  uint8_t next = (uint8_t)((rxHead + 1) % (sizeof(rxQueue) / sizeof(rxQueue[0])));
  if (next == rxTail) {
    return;
  }
  RxPacket& packet = rxQueue[rxHead];
  strncpy(packet.peer, peer != nullptr ? peer : "?", sizeof(packet.peer) - 1);
  packet.peer[sizeof(packet.peer) - 1] = '\0';
  packet.len = length > sizeof(packet.data) ? sizeof(packet.data) : (uint8_t)length;
  packet.channel = channel;
  memcpy(packet.data, data, packet.len);
  __sync_synchronize();
  rxHead = next;
#else
  (void)peer;
  (void)data;
  (void)length;
  (void)channel;
#endif
}

void pushMidiView(const uint8_t* bytes, uint8_t len) {
#if !USE_DISPLAY
  (void)bytes;
  (void)len;
#else
  if (bytes == nullptr || len == 0) {
    return;
  }
  if (len > 3) {
    bytes += len - 3;
    len = 3;
  }

  char text[12];
  int pos = 0;
  text[0] = '\0';
  for (uint8_t i = 0; i < len; i++) {
    int wrote = snprintf(text + pos, sizeof(text) - (size_t)pos, i == 0 ? "%02X" : " %02X", bytes[i]);
    if (wrote < 0) {
      break;
    }
    pos += wrote;
    if ((size_t)pos >= sizeof(text)) {
      break;
    }
  }

  uint8_t slot = midiLogWrite % (sizeof(midiLog) / sizeof(midiLog[0]));
  strncpy(midiLog[slot].text, text, sizeof(midiLog[slot].text) - 1);
  midiLog[slot].text[sizeof(midiLog[slot].text) - 1] = '\0';
  __sync_synchronize();
  midiLogWrite = (uint8_t)((midiLogWrite + 1) % (sizeof(midiLog) / sizeof(midiLog[0])));
  if (midiLogCount < sizeof(midiLog) / sizeof(midiLog[0])) {
    midiLogCount++;
  }
  displayDirty = true;
#endif
}

void applyMidiChannel(uint8_t* data, size_t length, uint8_t channel) {
  if (data == nullptr || length < 2 || channel < 1 || channel > 16) {
    return;
  }

  uint8_t nibble = (uint8_t)(channel - 1);
  uint8_t running = 0;
  size_t i = 1;

  while (i < length) {
    if ((data[i] & 0x80) == 0) {
      i++;
      continue;
    }

    // A BLE-MIDI timestamp byte precedes every message.
    if (data[i] >= 0xF8 && (i + 1 >= length || (data[i + 1] & 0x80) != 0)) {
      i++;
      continue;
    }
    i++;
    if (i >= length) {
      break;
    }

    uint8_t status;
    uint8_t msg[3];
    uint8_t msgLen = 0;
    if (data[i] & 0x80) {
      if (data[i] < 0xF0) {
        data[i] = (uint8_t)((data[i] & 0xF0) | nibble);
      }
      status = data[i];
      running = status < 0xF0 ? status : 0;
      i++;
      if (status >= 0xF8) {
        continue;
      }
      if (status < 0xF0) {
        msg[0] = status;
        msgLen = 1;
      }
    } else if (running >= 0x80 && running < 0xF0) {
      status = running;
      msg[0] = status;
      msgLen = 1;
    } else {
      break;
    }

    int8_t need = midiDataLength(status);
    if (need < 0) {
      while (i < length && data[i] != 0xF7) {
        if ((data[i] & 0x80) != 0 && data[i] < 0xF8) {
          i++;
          continue;
        }
        i++;
      }
      if (i < length && data[i] == 0xF7) {
        i++;
      }
      running = 0;
      continue;
    }

    while (need > 0 && i < length && (data[i] & 0x80) == 0) {
      if (msgLen < sizeof(msg)) {
        msg[msgLen++] = data[i];
      }
      i++;
      need--;
    }
    if (msgLen > 0) {
      pushMidiView(msg, msgLen);
      enqueueMidiCmd(msg, msgLen);
      midiLedPulse();
    }
  }
}

void acceptMidi(const char* peer, const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) {
    return;
  }
  uint8_t local[128];
  if (length > sizeof(local)) {
    length = sizeof(local);
  }
  memcpy(local, data, length);
  uint8_t channel = channelFor(peer);
  applyMidiChannel(local, length, channel);
  enqueueDebugRx(peer, local, length, channel);
}

void drainSerial() {
#if SERIAL_MODE == 1
  while (midiCmdTail != midiCmdHead) {
    __sync_synchronize();
    MidiCmd cmd = midiCmdQueue[midiCmdTail];
    midiCmdTail = (uint8_t)((midiCmdTail + 1) % (sizeof(midiCmdQueue) / sizeof(midiCmdQueue[0])));
    Serial.write(cmd.data, cmd.len);
  }
#elif SERIAL_MODE == 2
  while (rxTail != rxHead) {
    __sync_synchronize();
    RxPacket packet = rxQueue[rxTail];
    rxTail = (uint8_t)((rxTail + 1) % (sizeof(rxQueue) / sizeof(rxQueue[0])));
    Serial.printf("RX %s K%u:", packet.peer, packet.channel);
    for (uint8_t i = 0; i < packet.len; i++) {
      Serial.printf(" %02X", packet.data[i]);
    }
    Serial.println();
  }
#endif
}

void drawScreen() {
#if USE_DISPLAY
  if (!oledOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("Verbunden %u/%u", usedLinks(), MAX_LINKS);
  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

  uint8_t count = midiLogCount;
  uint8_t write = midiLogWrite;
  uint8_t show = count > 6 ? 6 : count;
  char rows[6][12];
  for (uint8_t row = 0; row < show; row++) {
    uint8_t age = (uint8_t)(show - 1 - row);
    uint8_t idx = (uint8_t)((write + 8 - 1 - age) % 8);
    strncpy(rows[row], midiLog[idx].text, sizeof(rows[row]) - 1);
    rows[row][sizeof(rows[row]) - 1] = '\0';
  }
  for (uint8_t row = 0; row < show; row++) {
    display.setCursor(0, 12 + row * 8);
    display.print(rows[row]);
  }
  display.display();
#endif
}

void refreshAdvertising() {
  if (pServer == nullptr) {
    return;
  }

  // Advertising beside an open MIDI link steals radio time,
  // and the peer disconnects with reason 520.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  bool want = outgoingCount() == 0 && !pendingReady && clientCount() < MAX_LINKS && usedLinks() < MAX_LINKS;
  if (want) {
    if (!advertising->isAdvertising() && advertising->start()) {
      DBG_PRINT("Advertising an, Clients %u/%u\n", clientCount(), MAX_LINKS);
    }
  } else if (advertising->isAdvertising()) {
    advertising->stop();
  }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    advDirty = true;
    std::string addr = pClient->getPeerAddress().toString();
    DBG_PRINT("MIDI %u/%u verbunden: %s, Kanal %u\n",
                  outgoingCount(),
                  MAX_LINKS,
                  addr.c_str(),
                  channelFor(addr.c_str()));
  }

  void onConnectFail(NimBLEClient* pClient, int reason) override {
    DBG_PRINT("MIDI verbinden fehlgeschlagen, Grund %d, %s\n", reason, pClient->getPeerAddress().toString().c_str());
    needScan = true;
    advDirty = true;
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    DBG_PRINT("MIDI getrennt: %s, Grund %d%s\n",
                  pClient->getPeerAddress().toString().c_str(),
                  reason,
                  reason == BLE_TIMEOUT ? " (Timeout)" : "");
    needScan = true;
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
    DBG_PRINT("MIDI gefunden: %s %s RSSI %d\n",
                  foundName.c_str(),
                  foundAddr.c_str(),
                  advertisedDevice->getRSSI());
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
    DBG_PRINT("Client %u/%u verbunden: %s\n",
                  server->getConnectedCount(),
                  MAX_LINKS,
                  connInfo.getAddress().toString().c_str());
    server->updateConnParams(connInfo.getConnHandle(), 24, 40, 0, 600);
    advDirty = true;
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    DBG_PRINT("Client getrennt: %s, Grund %d, noch %u/%u\n",
                  connInfo.getAddress().toString().c_str(),
                  reason,
                  server->getConnectedCount(),
                  MAX_LINKS);
    needScan = true;
    advDirty = true;
  }
} serverCallbacks;

class MidiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = pCharacteristic->getValue();
    std::string peer = connInfo.getAddress().toString();
    acceptMidi(peer.c_str(), value.data(), value.size());
  }
} midiCallbacks;

void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  (void)isNotify;
  std::string peer = pRemoteCharacteristic->getClient()->getPeerAddress().toString();
  acceptMidi(peer.c_str(), pData, length);
}

bool connectToMidi(const NimBLEAddress& address) {
  if (isLinked(address) || usedLinks() >= MAX_LINKS) {
    return false;
  }

  NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(address);
  if (pClient == nullptr) {
    pClient = NimBLEDevice::getDisconnectedClient();
  }
  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    if (pClient == nullptr) {
      DBG_PRINTLN("Kein BLE-Client frei");
      return false;
    }
  }

  pClient->setClientCallbacks(&clientCallbacks, false);
  pClient->setConnectTimeout(8000);
  // Devices that are already connected need radio time during this connect.
  uint16_t scanInterval = outgoingCount() > 0 ? 80 : 16;
  pClient->setConnectionParams(24, 40, 0, 600, scanInterval, 16);

  if (!pClient->isConnected() && !pClient->connect(address)) {
    DBG_PRINT("Verbindung abgelehnt: %s\n", address.toString().c_str());
    return false;
  }

  NimBLERemoteService* pSvc = pClient->getService(midiServiceUuid);
  if (pSvc == nullptr) {
    DBG_PRINTLN("MIDI-Service fehlt");
    pClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* pChr = pSvc->getCharacteristic(midiCharUuid);
  if (pChr == nullptr) {
    DBG_PRINTLN("MIDI-Characteristic fehlt");
    pClient->disconnect();
    return false;
  }

  bool notifications = pChr->canNotify() || !pChr->canIndicate();
  bool subscribed = pChr->subscribe(notifications, notifyCB, true);
  if (!subscribed) {
    subscribed = pChr->subscribe(notifications, notifyCB, false);
  }
  if (!subscribed) {
    DBG_PRINTLN("Notify fehlgeschlagen");
    pClient->disconnect();
    return false;
  }

  DBG_PRINT("MIDI-Notify aktiv: %s\n", address.toString().c_str());
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

  if (outgoingCount() > 0 || clientCount() > 0) {
    scan->setInterval(240);
    scan->setWindow(40);
  } else {
    scan->setInterval(100);
    scan->setWindow(80);
  }

  if (!scan->start(0, false, true)) {
    needScan = true;
    return;
  }
  DBG_PRINT("Suche MIDI-Geraete... %u/%u\n", outgoingCount(), MAX_LINKS);
}

void setup() {
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

#if USE_DISPLAY
  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
  if (!oledOk) {
    DBG_PRINTLN("OLED nicht gefunden");
  }
  drawScreen();
#endif

  NimBLEDevice::init("LeMidiReci");
  NimBLEDevice::setPower(9);
  NimBLEDevice::setMTU(128);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService* midiService = pServer->createService(midiServiceUuid);
  NimBLECharacteristic* midiChar = midiService->createCharacteristic(
      midiCharUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  midiChar->setValue((uint8_t)0x80);
  midiChar->setCallbacks(&midiCallbacks);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->setMinInterval(320);
  advertising->setMaxInterval(480);
  advertising->setName("LeMidiReci");
  advertising->addServiceUUID(midiServiceUuid);
  if (!advertising->start()) {
    DBG_PRINTLN("Advertising fehlgeschlagen");
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  startScan();
}

void loop() {
  midiLedPoll();
  drainSerial();

  bool justConnected = false;
  if (pendingReady) {
    __sync_synchronize();
    NimBLEAddress address = pendingAddress;
    pendingReady = false;
    if (NimBLEDevice::getAdvertising()->isAdvertising()) {
      NimBLEDevice::getAdvertising()->stop();
    }
    if (connectToMidi(address) && usedLinks() < MAX_LINKS) {
      nextScanAt = millis() + 1000;
      needScan = false;
      justConnected = true;
    } else if (usedLinks() < MAX_LINKS) {
      needScan = true;
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
  if ((displayDirty || linksChanged) && (linksChanged || now - lastDraw >= 80)) {
    displayDirty = false;
    shownLinks = links;
    lastDraw = now;
    drawScreen();
  }
#endif

  delay(20);
}
