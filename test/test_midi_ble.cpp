#include <stdio.h>
#include <string.h>

#include "../midi_ble.h"

static int failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);              \
      failures++;                                                          \
    }                                                                      \
  } while (0)

typedef struct Capture {
  MidiMessage msg[16];
  int count;
} Capture;

static void onMsg(const MidiMessage* message, void* context) {
  Capture* capture = (Capture*)context;
  if (capture->count < 16) {
    capture->msg[capture->count++] = *message;
  }
}

static void parseOne(MidiParser* parser, const uint8_t* packet, size_t length, uint8_t channel, Capture* capture) {
  midiParsePacket(parser, packet, length, channel, onMsg, capture);
}

static bool bytesEq(const MidiMessage* message, const uint8_t* bytes, uint16_t length) {
  return message->length == length && memcmp(message->data, bytes, length) == 0;
}

static void testChannelMap() {
  static const DeviceChannel table[] = {
      {"SMC-PAD Pocket", 2},
      {"SMC-PAD", 3},
      {"aa:bb:cc:dd:ee:ff", 5},
      {"Bad", 0},
      {"AlsoBad", 17},
  };
  EXPECT(midiChannelFor("11:22:33:44:55:66", "SMC-PAD Pocket", table, 5) == 2);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "smc-pad pocket", table, 5) == 2);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "SMC-PAD", table, 5) == 3);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "SMC-PAD Pocket Extra", table, 5) == 1);
  EXPECT(midiChannelFor("AA:BB:CC:DD:EE:FF", "SMC-PAD", table, 5) == 5);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "unknown", table, 5) == 1);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "Bad", table, 5) == 1);
  EXPECT(midiChannelFor("11:22:33:44:55:66", "AlsoBad", table, 5) == 1);
  EXPECT(midiChannelFor(nullptr, nullptr, table, 5) == 1);
}

static void testNotesAndShortMessages() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));

  const uint8_t noteOn[] = {0x80, 0x80, 0x90, 0x24, 0x46};
  parseOne(&parser, noteOn, sizeof(noteOn), 3, &capture);
  EXPECT(capture.count == 1);
  const uint8_t noteOnOut[] = {0x92, 0x24, 0x46};
  EXPECT(bytesEq(&capture.msg[0], noteOnOut, 3));
  EXPECT(capture.msg[0].originalChannel == 1);
  EXPECT(capture.msg[0].targetChannel == 3);

  capture.count = 0;
  const uint8_t noteOff[] = {0x80, 0x81, 0x80, 0x24, 0x40};
  parseOne(&parser, noteOff, sizeof(noteOff), 2, &capture);
  const uint8_t noteOffOut[] = {0x81, 0x24, 0x40};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], noteOffOut, 3));

  capture.count = 0;
  const uint8_t velocity0[] = {0x80, 0x82, 0x90, 0x24, 0x00};
  parseOne(&parser, velocity0, sizeof(velocity0), 3, &capture);
  const uint8_t velocity0Out[] = {0x92, 0x24, 0x00};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], velocity0Out, 3));

  capture.count = 0;
  const uint8_t program[] = {0x80, 0x83, 0xC0, 0x05};
  parseOne(&parser, program, sizeof(program), 3, &capture);
  const uint8_t programOut[] = {0xC2, 0x05};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], programOut, 2));

  capture.count = 0;
  const uint8_t pressure[] = {0x80, 0x84, 0xD0, 0x40};
  parseOne(&parser, pressure, sizeof(pressure), 3, &capture);
  const uint8_t pressureOut[] = {0xD2, 0x40};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], pressureOut, 2));

  capture.count = 0;
  const uint8_t bend[] = {0x80, 0x85, 0xE0, 0x00, 0x40};
  parseOne(&parser, bend, sizeof(bend), 4, &capture);
  const uint8_t bendOut[] = {0xE3, 0x00, 0x40};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], bendOut, 3));
  EXPECT(parser.stats.errors == 0);
}

static void testRunningStatusAndBundle() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));

  const uint8_t packet[] = {0x80, 0x80, 0x90, 0x24, 0x46, 0x26, 0x40, 0x81, 0xB0, 0x07, 0x7F};
  parseOne(&parser, packet, sizeof(packet), 3, &capture);
  EXPECT(capture.count == 3);
  const uint8_t first[] = {0x92, 0x24, 0x46};
  const uint8_t second[] = {0x92, 0x26, 0x40};
  const uint8_t third[] = {0xB2, 0x07, 0x7F};
  EXPECT(bytesEq(&capture.msg[0], first, 3));
  EXPECT(bytesEq(&capture.msg[1], second, 3));
  EXPECT(bytesEq(&capture.msg[2], third, 3));
  EXPECT(capture.msg[1].originalChannel == 1);
  EXPECT(capture.msg[2].originalChannel == 1);

  Capture next;
  memset(&next, 0, sizeof(next));
  const uint8_t cancelled[] = {0x80, 0x26, 0x41};
  parseOne(&parser, cancelled, sizeof(cancelled), 3, &next);
  EXPECT(next.count == 0);
  EXPECT(parser.stats.errors >= 1);
}

static void testRealtimeBetweenData() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));
  const uint8_t packet[] = {0x80, 0x80, 0x90, 0x24, 0x81, 0xF8, 0x46, 0x82, 0xFA, 0x83, 0xFC, 0x84, 0xFB, 0x85, 0xFE};
  parseOne(&parser, packet, sizeof(packet), 2, &capture);
  EXPECT(capture.count == 6);
  EXPECT(capture.msg[0].length == 1 && capture.msg[0].data[0] == 0xF8);
  const uint8_t note[] = {0x91, 0x24, 0x46};
  EXPECT(bytesEq(&capture.msg[1], note, 3));
  EXPECT(capture.msg[2].data[0] == 0xFA);
  EXPECT(capture.msg[3].data[0] == 0xFC);
  EXPECT(capture.msg[4].data[0] == 0xFB);
  EXPECT(capture.msg[5].data[0] == 0xFE);
  EXPECT(capture.msg[0].originalChannel == 0);
  EXPECT(capture.msg[1].targetChannel == 2);
}

static void testSystemAndSysex() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));
  const uint8_t common[] = {0x80, 0x80, 0xF2, 0x01, 0x02, 0x81, 0xF1, 0x10, 0x82, 0xF3, 0x04, 0x83, 0xF6};
  parseOne(&parser, common, sizeof(common), 8, &capture);
  EXPECT(capture.count == 4);
  const uint8_t song[] = {0xF2, 0x01, 0x02};
  const uint8_t quarter[] = {0xF1, 0x10};
  const uint8_t select[] = {0xF3, 0x04};
  EXPECT(bytesEq(&capture.msg[0], song, 3));
  EXPECT(bytesEq(&capture.msg[1], quarter, 2));
  EXPECT(bytesEq(&capture.msg[2], select, 2));
  EXPECT(capture.msg[3].data[0] == 0xF6 && capture.msg[3].length == 1);
  EXPECT(capture.msg[0].originalChannel == 0);
  EXPECT(capture.msg[0].targetChannel == 0);

  capture.count = 0;
  const uint8_t part1[] = {0x80, 0x80, 0xF0, 0x01, 0x02, 0x03};
  const uint8_t part2[] = {0x80, 0x04, 0x05, 0x81, 0xF7};
  parseOne(&parser, part1, sizeof(part1), 4, &capture);
  EXPECT(capture.count == 0);
  EXPECT(parser.inSysex);
  parseOne(&parser, part2, sizeof(part2), 4, &capture);
  const uint8_t sysex[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], sysex, 7));
  EXPECT(!parser.inSysex);

  capture.count = 0;
  const uint8_t interrupted[] = {0x80, 0x80, 0xF0, 0x11, 0x82, 0xF8, 0x12, 0x83, 0xF7};
  parseOne(&parser, interrupted, sizeof(interrupted), 4, &capture);
  EXPECT(capture.count == 2);
  EXPECT(capture.msg[0].data[0] == 0xF8);
  const uint8_t sysex2[] = {0xF0, 0x11, 0x12, 0xF7};
  EXPECT(bytesEq(&capture.msg[1], sysex2, 4));
}

static void testTimestampWrap() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));
  const uint8_t packet[] = {0x80, 0xFF, 0x90, 0x24, 0x40, 0x80, 0x90, 0x26, 0x41};
  parseOne(&parser, packet, sizeof(packet), 1, &capture);
  EXPECT(capture.count == 2);
  EXPECT(capture.msg[0].timestamp == 127);
  EXPECT(!capture.msg[0].timestampWrapped);
  EXPECT(capture.msg[1].timestamp == 128);
  EXPECT(capture.msg[1].timestampWrapped);
  EXPECT(parser.stats.timestampWraps == 1);

  MidiParser across;
  midiParserInit(&across);
  Capture first;
  memset(&first, 0, sizeof(first));
  const uint8_t high[] = {0xBF, 0xFF, 0x90, 0x24, 0x40};
  parseOne(&across, high, sizeof(high), 1, &first);
  EXPECT(first.count == 1);
  EXPECT(first.msg[0].timestamp == 8191);
  EXPECT(across.stats.timestampWraps == 0);
  Capture second;
  memset(&second, 0, sizeof(second));
  const uint8_t low[] = {0x80, 0x80, 0x90, 0x24, 0x40};
  parseOne(&across, low, sizeof(low), 1, &second);
  EXPECT(second.count == 1);
  EXPECT(second.msg[0].timestamp == 0);
  EXPECT(second.msg[0].timestampWrapped);
  EXPECT(across.stats.timestampWraps == 1);
}

static void testInvalidAndRecovery() {
  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));
  const uint8_t badHeader[] = {0x00, 0x80, 0x90, 0x24, 0x40};
  parseOne(&parser, badHeader, sizeof(badHeader), 1, &capture);
  EXPECT(capture.count == 0);

  const uint8_t truncated[] = {0x80, 0x80, 0x90, 0x24};
  parseOne(&parser, truncated, sizeof(truncated), 1, &capture);
  EXPECT(capture.count == 0);

  const uint8_t undefined[] = {0x80, 0x80, 0xF4, 0x81, 0xF5};
  parseOne(&parser, undefined, sizeof(undefined), 1, &capture);
  EXPECT(capture.count == 0);

  capture.count = 0;
  uint16_t errorsBefore = parser.stats.errors;
  const uint8_t recovered[] = {0x80, 0x80, 0x11, 0x22, 0x81, 0x90, 0x30, 0x40};
  parseOne(&parser, recovered, sizeof(recovered), 5, &capture);
  const uint8_t note[] = {0x94, 0x30, 0x40};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], note, 3));
  EXPECT(parser.stats.errors > errorsBefore);

  capture.count = 0;
  uint8_t overflow[4 + 130];
  overflow[0] = 0x80;
  overflow[1] = 0x80;
  overflow[2] = 0xF0;
  for (int i = 0; i < 130; i++) {
    overflow[3 + i] = 0x01;
  }
  parseOne(&parser, overflow, sizeof(overflow), 1, &capture);
  const uint8_t endOverflow[] = {0x80, 0x81, 0xF7};
  parseOne(&parser, endOverflow, sizeof(endOverflow), 1, &capture);
  EXPECT(capture.count == 0);
  EXPECT(parser.stats.sysexOverflows >= 1);
  EXPECT(!parser.inSysex);

  const uint8_t after[] = {0x80, 0x82, 0x90, 0x01, 0x02};
  parseOne(&parser, after, sizeof(after), 6, &capture);
  const uint8_t afterOut[] = {0x95, 0x01, 0x02};
  EXPECT(capture.count == 1);
  EXPECT(bytesEq(&capture.msg[0], afterOut, 3));
}

static void testAllChannels() {
  for (uint8_t channel = 1; channel <= 16; channel++) {
    MidiParser parser;
    midiParserInit(&parser);
    Capture capture;
    memset(&capture, 0, sizeof(capture));
    uint8_t packet[] = {0x80, 0x80, (uint8_t)(0x90 | (channel - 1)), 0x10, 0x20};
    parseOne(&parser, packet, sizeof(packet), channel, &capture);
    EXPECT(capture.count == 1);
    EXPECT(capture.msg[0].data[0] == packet[2]);
    EXPECT(capture.msg[0].originalChannel == channel);
    EXPECT(capture.msg[0].targetChannel == channel);
    EXPECT(capture.msg[0].data[1] == 0x10);
    EXPECT(capture.msg[0].data[2] == 0x20);
  }

  MidiParser parser;
  midiParserInit(&parser);
  Capture capture;
  memset(&capture, 0, sizeof(capture));
  const uint8_t packet[] = {0x80, 0x80, 0x9F, 0x10, 0x20};
  parseOne(&parser, packet, sizeof(packet), 1, &capture);
  EXPECT(capture.msg[0].data[0] == 0x90);
  EXPECT(capture.msg[0].originalChannel == 16);
  EXPECT(capture.msg[0].targetChannel == 1);

  capture.count = 0;
  parseOne(&parser, packet, sizeof(packet), 0, &capture);
  EXPECT(capture.msg[0].targetChannel == 1);
  EXPECT(capture.msg[0].data[0] == 0x90);
}

static void testSeparateParsers() {
  MidiParser pad;
  MidiParser pocket;
  midiParserInit(&pad);
  midiParserInit(&pocket);
  Capture padNotes;
  Capture pocketNotes;
  memset(&padNotes, 0, sizeof(padNotes));
  memset(&pocketNotes, 0, sizeof(pocketNotes));

  const uint8_t openSysex[] = {0x80, 0x80, 0xF0, 0x7E};
  const uint8_t pocketNote[] = {0x80, 0x80, 0x90, 0x30, 0x40};
  const uint8_t closeSysex[] = {0x80, 0x00, 0x81, 0xF7};
  parseOne(&pad, openSysex, sizeof(openSysex), 3, &padNotes);
  parseOne(&pocket, pocketNote, sizeof(pocketNote), 2, &pocketNotes);
  EXPECT(pad.inSysex);
  EXPECT(!pocket.inSysex);
  EXPECT(pocketNotes.count == 1);
  const uint8_t pocketOut[] = {0x91, 0x30, 0x40};
  EXPECT(bytesEq(&pocketNotes.msg[0], pocketOut, 3));
  parseOne(&pad, closeSysex, sizeof(closeSysex), 3, &padNotes);
  const uint8_t sysex[] = {0xF0, 0x7E, 0x00, 0xF7};
  EXPECT(padNotes.count == 1);
  EXPECT(bytesEq(&padNotes.msg[0], sysex, 4));
  EXPECT(pocket.stats.messages == 1);
  EXPECT(pad.stats.messages == 1);
}

static void testFanout() {
  uint8_t uartStorage[8];
  uint8_t usbStorage[8];
  MidiByteQueue uart;
  MidiByteQueue usb;
  midiQueueInit(&uart, uartStorage, sizeof(uartStorage));
  midiQueueInit(&usb, usbStorage, sizeof(usbStorage));
  const uint8_t note[] = {0x92, 0x24, 0x46};
  bool uartSent = false;
  bool usbSent = false;

  MidiSinkMask both = {true, true};
  midiFanout(note, 3, both, &uart, &usb, &uartSent, &usbSent);
  EXPECT(uartSent && usbSent);
  EXPECT(midiQueueCount(&uart) == 3);
  EXPECT(midiQueueCount(&usb) == 3);

  MidiSinkMask uartOnly = {true, false};
  const uint8_t clock[] = {0xF8};
  midiFanout(clock, 1, uartOnly, &uart, &usb, &uartSent, &usbSent);
  EXPECT(uartSent);
  EXPECT(!usbSent);
  EXPECT(midiQueueCount(&uart) == 4);
  EXPECT(midiQueueCount(&usb) == 3);

  uint8_t dumped[8];
  EXPECT(midiQueuePop(&usb, dumped, 8) == 3);
  EXPECT(memcmp(dumped, note, 3) == 0);
  EXPECT(midiQueueCount(&usb) == 0);

  MidiSinkMask none = {false, false};
  midiFanout(note, 3, none, &uart, &usb, &uartSent, &usbSent);
  EXPECT(!uartSent && !usbSent);
  EXPECT(midiQueueCount(&usb) == 0);

  // capacity 8 holds 7 bytes. 4 are already queued, so a 3-byte message fits and the next does not.
  EXPECT(midiQueuePush(&uart, note, 3));
  EXPECT(midiQueueCount(&uart) == 7);
  EXPECT(!midiQueuePush(&uart, note, 3));
  EXPECT(uart.drops == 1);
  EXPECT(midiQueueCount(&uart) == 7);
  EXPECT(midiQueuePop(&uart, dumped, 1) == 1);
  EXPECT(dumped[0] == 0x92);
}

int main() {
  testChannelMap();
  testNotesAndShortMessages();
  testRunningStatusAndBundle();
  testRealtimeBetweenData();
  testSystemAndSysex();
  testTimestampWrap();
  testInvalidAndRecovery();
  testAllChannels();
  testSeparateParsers();
  testFanout();
  if (failures != 0) {
    printf("%d failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
