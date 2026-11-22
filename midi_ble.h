#pragma once

// BLE-MIDI 1.0 packet parser (MMA / Apple, November 2015).
// No Arduino types, so the same code runs in the sketch and in host tests.
//
// A packet is a header (bit 7 set, bit 6 reserved, bits 5-0 timestampHigh)
// followed by timestamp bytes (bit 7 set, bits 6-0 timestampLow) and MIDI bytes.
// The 13-bit timestamp is (timestampHigh << 7) | timestampLow.
// Inside one packet, a smaller timestampLow than the previous one means
// timestampHigh increments by one. That increment is not transmitted.
// Across packets the header already carries the new high bits, so a value
// lower than the previous packet's timestamp is one 8192 ms wrap.
//
// Running status is restored for the caller and is cancelled at the end of
// every packet. System common and system real-time do not cancel it inside
// the packet. Only SysEx may continue in the next packet. A continuation
// packet is a header followed immediately by a data byte.
// A status byte is preceded by a timestamp. Real-time bytes may sit between
// the data bytes of another message; they are emitted when they are seen.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__cplusplus)
#include <stdbool.h>
#ifndef nullptr
#define nullptr NULL
#endif
#endif

#define MIDI_SYSEX_MAX 128

typedef struct MidiMessage {
  uint8_t data[MIDI_SYSEX_MAX];
  uint16_t length;
  uint8_t originalChannel;
  uint8_t targetChannel;
  uint16_t timestamp;
  bool timestampWrapped;
} MidiMessage;

typedef struct MidiParseStats {
  uint16_t messages;
  uint16_t errors;
  uint16_t sysexOverflows;
  uint16_t timestampWraps;
} MidiParseStats;

typedef struct MidiParser {
  bool inSysex;
  bool sysexDrop;
  uint8_t sysex[MIDI_SYSEX_MAX];
  uint16_t sysexLen;
  uint16_t timestamp;
  uint16_t lastPacketTimestamp;
  bool havePacketTimestamp;
  MidiParseStats stats;
} MidiParser;

typedef void (*MidiEmitFn)(const MidiMessage* message, void* context);

typedef struct DeviceChannel {
  const char* id;
  uint8_t channel;
} DeviceChannel;

typedef struct MidiByteQueue {
  uint8_t* data;
  uint16_t capacity;
  volatile uint16_t head;
  volatile uint16_t tail;
  uint16_t drops;
} MidiByteQueue;

typedef struct MidiSinkMask {
  bool uart;
  bool usbBinary;
} MidiSinkMask;

#if defined(_MSC_VER)
#include <intrin.h>
#define MIDI_BARRIER() _ReadWriteBarrier()
#elif defined(__TINYC__)
#define MIDI_BARRIER()
#elif defined(__GNUC__)
#define MIDI_BARRIER() __sync_synchronize()
#else
#define MIDI_BARRIER()
#endif

static inline void midiStatInc(uint16_t* value) {
  if (*value < 0xFFFF) {
    (*value)++;
  }
}

static inline void midiParserInit(MidiParser* parser) {
  if (parser == nullptr) {
    return;
  }
  memset(parser, 0, sizeof(*parser));
}

static inline bool midiSameText(const char* a, const char* b) {
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

// Address entries win over name entries. The first match wins.
// An entry outside 1..16, or no match, yields channel 1.
static inline uint8_t midiChannelFor(const char* address, const char* name,
                                     const DeviceChannel* table, size_t count) {
  if (table == nullptr) {
    return 1;
  }
  if (address != nullptr && address[0] != '\0') {
    for (size_t i = 0; i < count; i++) {
      if (midiSameText(table[i].id, address)) {
        uint8_t channel = table[i].channel;
        return channel >= 1 && channel <= 16 ? channel : 1;
      }
    }
  }
  if (name != nullptr && name[0] != '\0') {
    for (size_t i = 0; i < count; i++) {
      if (midiSameText(table[i].id, name)) {
        uint8_t channel = table[i].channel;
        return channel >= 1 && channel <= 16 ? channel : 1;
      }
    }
  }
  return 1;
}

static inline int8_t midiDataLength(uint8_t status) {
  if (status >= 0xF8 || status == 0xF6) {
    return 0;
  }
  if (status == 0xF1 || status == 0xF3) {
    return 1;
  }
  if (status == 0xF2) {
    return 2;
  }
  if (status < 0xF0) {
    uint8_t kind = (uint8_t)(status & 0xF0);
    if (kind == 0xC0 || kind == 0xD0) {
      return 1;
    }
    if (kind >= 0x80) {
      return 2;
    }
  }
  return -1;
}

static inline void midiEmit(MidiEmitFn emit, void* context, MidiParser* parser,
                            const uint8_t* bytes, uint16_t length, uint8_t targetChannel,
                            bool wrapped) {
  if (parser == nullptr || emit == nullptr || bytes == nullptr || length == 0 || length > MIDI_SYSEX_MAX) {
    if (parser != nullptr) {
      midiStatInc(&parser->stats.errors);
    }
    return;
  }
  MidiMessage message;
  memset(&message, 0, sizeof(message));
  memcpy(message.data, bytes, length);
  message.length = length;
  message.timestamp = parser->timestamp;
  message.timestampWrapped = wrapped;
  if (bytes[0] >= 0x80 && bytes[0] < 0xF0) {
    message.originalChannel = (uint8_t)((bytes[0] & 0x0F) + 1);
    uint8_t target = targetChannel;
    if (target < 1 || target > 16) {
      target = 1;
    }
    message.targetChannel = target;
    message.data[0] = (uint8_t)((bytes[0] & 0xF0) | (target - 1));
  }
  midiStatInc(&parser->stats.messages);
  emit(&message, context);
}

static inline bool midiAppendSysex(MidiParser* parser, uint8_t byte) {
  if (parser->sysexDrop) {
    return false;
  }
  // Keep the last byte free so a following F7 still fits in a message that is emitted.
  if (parser->sysexLen >= MIDI_SYSEX_MAX - 1) {
    parser->sysexDrop = true;
    midiStatInc(&parser->stats.sysexOverflows);
    return false;
  }
  parser->sysex[parser->sysexLen++] = byte;
  return true;
}

static inline void midiParsePacket(MidiParser* parser, const uint8_t* data, size_t length,
                                   uint8_t targetChannel, MidiEmitFn emit, void* context) {
  if (parser == nullptr) {
    return;
  }
  if (data == nullptr || length == 0 || (data[0] & 0x80) == 0) {
    midiStatInc(&parser->stats.errors);
    return;
  }
  if ((data[0] & 0x40) != 0) {
    midiStatInc(&parser->stats.errors);
  }
  if (length < 2) {
    return;
  }

  bool continuation = (data[1] & 0x80) == 0;
  if (continuation && !parser->inSysex) {
    midiStatInc(&parser->stats.errors);
    return;
  }

  uint8_t tsHigh = (uint8_t)(data[0] & 0x3F);
  bool haveLow = false;
  uint8_t lastLow = 0;
  bool firstTimestamp = true;
  bool stampWrapped = false;
  uint8_t running = 0;
  bool building = false;
  bool resumeData = false;
  uint8_t build[3];
  uint8_t buildLen = 0;
  int8_t need = 0;
  // 0 = next MSB byte is a timestamp, 1 = next MSB byte is a status, 2 = collecting data.
  uint8_t phase = 0;

  for (size_t i = 1; i < length; i++) {
    uint8_t byte = data[i];

    if ((byte & 0x80) == 0) {
      if (parser->inSysex && !building) {
        midiAppendSysex(parser, byte);
        phase = 0;
        continue;
      }
      if (building) {
        if (buildLen < sizeof(build)) {
          build[buildLen++] = byte;
        }
        need--;
        if (need <= 0) {
          midiEmit(emit, context, parser, build, buildLen, targetChannel, stampWrapped);
          building = false;
          resumeData = false;
          phase = 0;
        } else {
          phase = 2;
        }
        continue;
      }
      if (running >= 0x80 && running < 0xF0 && !parser->inSysex) {
        if (resumeData) {
          midiStatInc(&parser->stats.errors);
          resumeData = false;
        }
        int8_t dataLen = midiDataLength(running);
        if (dataLen < 1) {
          midiStatInc(&parser->stats.errors);
          phase = 0;
          continue;
        }
        build[0] = running;
        build[1] = byte;
        buildLen = 2;
        need = (int8_t)(dataLen - 1);
        if (need <= 0) {
          midiEmit(emit, context, parser, build, buildLen, targetChannel, stampWrapped);
          phase = 0;
        } else {
          building = true;
          phase = 2;
        }
        continue;
      }
      midiStatInc(&parser->stats.errors);
      phase = 0;
      continue;
    }

    if (phase != 1) {
      uint8_t low = (uint8_t)(byte & 0x7F);
      stampWrapped = false;
      if (haveLow && low < lastLow) {
        tsHigh = (uint8_t)((tsHigh + 1) & 0x3F);
        midiStatInc(&parser->stats.timestampWraps);
        stampWrapped = true;
      }
      lastLow = low;
      haveLow = true;
      uint16_t timestamp = (uint16_t)(((uint16_t)tsHigh << 7) | low);
      if (firstTimestamp && parser->havePacketTimestamp && timestamp < parser->lastPacketTimestamp) {
        midiStatInc(&parser->stats.timestampWraps);
        stampWrapped = true;
      }
      firstTimestamp = false;
      parser->timestamp = timestamp;
      parser->lastPacketTimestamp = timestamp;
      parser->havePacketTimestamp = true;
      if (phase == 2 && building) {
        resumeData = true;
      }
      phase = 1;
      continue;
    }

    phase = 0;
    if (byte >= 0xF8) {
      midiEmit(emit, context, parser, &byte, 1, targetChannel, stampWrapped);
      phase = (resumeData && building) ? 2 : 0;
      resumeData = false;
      continue;
    }
    if (byte == 0xF7) {
      if (parser->inSysex && !parser->sysexDrop && parser->sysexLen < MIDI_SYSEX_MAX) {
        parser->sysex[parser->sysexLen++] = 0xF7;
        midiEmit(emit, context, parser, parser->sysex, parser->sysexLen, targetChannel, stampWrapped);
      } else if (parser->inSysex && parser->sysexDrop) {
        // The overflow was already counted. Do not forward a cut-off SysEx.
      } else {
        midiStatInc(&parser->stats.errors);
      }
      parser->inSysex = false;
      parser->sysexDrop = false;
      parser->sysexLen = 0;
      if (building) {
        midiStatInc(&parser->stats.errors);
        building = false;
      }
      resumeData = false;
      continue;
    }
    if (byte == 0xF4 || byte == 0xF5) {
      midiStatInc(&parser->stats.errors);
      if (building) {
        building = false;
      }
      resumeData = false;
      continue;
    }
    if (building) {
      midiStatInc(&parser->stats.errors);
      building = false;
      resumeData = false;
    }
    if (byte == 0xF0) {
      if (parser->inSysex) {
        midiStatInc(&parser->stats.errors);
      }
      parser->inSysex = true;
      parser->sysexDrop = false;
      parser->sysexLen = 0;
      midiAppendSysex(parser, 0xF0);
      continue;
    }

    int8_t dataLen = midiDataLength(byte);
    if (byte < 0xF0) {
      running = byte;
    }
    if (dataLen < 0) {
      midiStatInc(&parser->stats.errors);
      continue;
    }
    if (dataLen == 0) {
      midiEmit(emit, context, parser, &byte, 1, targetChannel, stampWrapped);
      continue;
    }
    build[0] = byte;
    buildLen = 1;
    need = dataLen;
    building = true;
    phase = 2;
  }

  if (building) {
    midiStatInc(&parser->stats.errors);
  }
}

static inline void midiQueueInit(MidiByteQueue* queue, uint8_t* storage, uint16_t capacity) {
  if (queue == nullptr) {
    return;
  }
  queue->data = storage;
  queue->capacity = capacity;
  queue->head = 0;
  queue->tail = 0;
  queue->drops = 0;
}

static inline uint16_t midiQueueCount(const MidiByteQueue* queue) {
  uint16_t head = queue->head;
  uint16_t tail = queue->tail;
  MIDI_BARRIER();
  if (head >= tail) {
    return (uint16_t)(head - tail);
  }
  return (uint16_t)(queue->capacity - tail + head);
}

// Copies the whole message or none of it. A full queue increments drops.
static inline bool midiQueuePush(MidiByteQueue* queue, const uint8_t* bytes, uint16_t length) {
  if (queue == nullptr || queue->data == nullptr || queue->capacity < 2 || bytes == nullptr || length == 0) {
    return false;
  }
  uint16_t head = queue->head;
  uint16_t tail = queue->tail;
  MIDI_BARRIER();
  uint16_t used = head >= tail ? (uint16_t)(head - tail) : (uint16_t)(queue->capacity - tail + head);
  uint16_t free = (uint16_t)(queue->capacity - 1 - used);
  if (length > free) {
    midiStatInc(&queue->drops);
    return false;
  }
  for (uint16_t i = 0; i < length; i++) {
    queue->data[head] = bytes[i];
    head = (uint16_t)((head + 1) % queue->capacity);
  }
  MIDI_BARRIER();
  queue->head = head;
  return true;
}

static inline uint16_t midiQueuePop(MidiByteQueue* queue, uint8_t* out, uint16_t max) {
  if (queue == nullptr || out == nullptr || max == 0) {
    return 0;
  }
  uint16_t head = queue->head;
  MIDI_BARRIER();
  uint16_t tail = queue->tail;
  uint16_t count = 0;
  while (count < max && tail != head) {
    out[count++] = queue->data[tail];
    tail = (uint16_t)((tail + 1) % queue->capacity);
  }
  MIDI_BARRIER();
  queue->tail = tail;
  return count;
}

// Sends one remapped MIDI message to the enabled outputs.
// A disabled output is left untouched. A full output keeps its old bytes.
static inline void midiFanout(const uint8_t* data, uint16_t length, MidiSinkMask mask,
                              MidiByteQueue* uart, MidiByteQueue* usb,
                              bool* uartSent, bool* usbSent) {
  if (uartSent != nullptr) {
    *uartSent = false;
  }
  if (usbSent != nullptr) {
    *usbSent = false;
  }
  if (mask.uart && uart != nullptr) {
    bool sent = midiQueuePush(uart, data, length);
    if (uartSent != nullptr) {
      *uartSent = sent;
    }
  }
  if (mask.usbBinary && usb != nullptr) {
    bool sent = midiQueuePush(usb, data, length);
    if (usbSent != nullptr) {
      *usbSent = sent;
    }
  }
}
