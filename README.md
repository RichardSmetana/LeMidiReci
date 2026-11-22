<p align="center">
  <img src="docs/logo.jpg" alt="LeMidiReci — BLE-MIDI Receiver" width="640">
</p>

# LeMidiReci

LeMidiReci is an ESP32 firmware that receives BLE-MIDI from up to three links, remaps each device onto a MIDI channel, and forwards the MIDI bytes. It runs on an ESP32-WROOM-32 and on an ESP32-C3 from the same sketch.

Forwarding can go to a hardware UART at the MIDI baud rate and, separately, to the USB serial port. Those two paths are not the same thing as a USB-MIDI class device or a DIN socket.

## Supported hardware

| | ESP32-WROOM-32 | ESP32-C3 |
|---|---|---|
| Arduino IDE board | ESP32 Dev Module | ESP32C3 Dev Module |
| USB serial | external USB-UART, usually a CP2102 on UART0 | native USB Serial/JTAG. Enable **USB CDC On Boot** |
| BLE controller links | 3 | 3, shared with scanning and advertising |
| OLED SDA / SCL | GPIO 5 / GPIO 4 | GPIO 5 / GPIO 4 |
| OLED | SSD1306 128×64, I²C `0x3C` | same |
| MIDI UART TX / RX | GPIO 17 / GPIO 16, UART2 (`Serial2`) | GPIO 7 / GPIO 6, UART1 (`Serial1`) |
| Activity LED | only if you set the pin | GPIO 8, and only if that board wires an LED there |

RX is reserved by the MIDI UART so the peripheral has a valid pin. The sketch does not read MIDI IN.

Photographs of the SuperMini, the WeMos-marked OLED board, the SMC-PAD, and the SMC-PAD Pocket, with the source for each pin claim, are in [docs/HARDWARE.md](docs/HARDWARE.md).

Leave this line commented so the IDE board menu selects the target:

```cpp
// #define BOARD_ESP32C3 0
```

Set it to `0` for the WROOM-32 or `1` for the C3 only when the IDE selection is being ignored.

## What it does

- Scans for the standard BLE-MIDI service and connects as a central to up to three peripherals.
- Advertises as a BLE-MIDI peripheral named **LeMidiReci** when it has no outgoing central link.
- Parses BLE-MIDI packets into MIDI 1.0 messages, including running status, system common, system real-time, and SysEx that spans packets.
- Remaps channel voice to a per-device channel. System messages and SysEx are not remapped.
- Sends the remapped bytes to a hardware UART at 31250 baud, 8 data bits, no parity, 1 stop bit.
- Sends the same bytes to USB serial when `SERIAL_MODE` is 1, or prints a text log when it is 2.
- Shows link and MIDI status on an optional SSD1306, and can pulse an optional LED.

USB serial mode 1 is a stream of MIDI bytes on a COM port. It is not a USB-MIDI class interface. A DAW or Sonic Pi needs a serial-to-MIDI bridge for that stream. The hardware UART is also not a MIDI DIN output until it is wired through a MIDI output circuit. A bare GPIO is a 3.3 V logic signal, idle high. The usual 5 V DIN circuit (two 220 ohm resistors from pin 4 and pin 5) does not match a 3.3 V pin, so use a buffer or a 3.3 V MIDI out circuit. That circuit was not built or measured for this project.

## Architecture

```text
BLE notify or GATT write
        |
        v
per-link BLE-MIDI parser  (midi_ble.h)
        |
        v
channel remap, then one MIDI message
        |
        +-- UART queue --> Serial2 (WROOM) or Serial1 (C3) at 31250
        +-- USB queue  --> Serial, only when SERIAL_MODE is 1
        +-- debug queue -> Serial text, only when SERIAL_MODE is 2
        +-- OLED log and LED, except MIDI clock and active sensing
```

The parser runs in the NimBLE callback. It only copies bytes into fixed queues. USB text, the UART, and the OLED are drained from `loop()`, so a slow port cannot sit inside the BLE callback.

Each BLE address has its own parser. Running status and an open SysEx on one device do not affect another device. Running status ends with the BLE packet, which is what the BLE-MIDI specification requires. SysEx is the message that is allowed to continue in the next packet.

The three controller links are one pool. An incoming phone and two outgoing pads are three links. While any outgoing central link is up, LeMidiReci stops advertising, because advertising next to that link has caused supervision timeouts (reason 520, HCI 0x08). When the last central link drops, advertising as **LeMidiReci** starts again. Scanning uses a short window while any link is up, and waits 500 ms after a disconnect before it starts again. A new central connection waits 1 s before the next scan. Those delays are not a claim that they remove radio timeouts. The connection interval is still 30–50 ms (`24` to `40` units of 1.25 ms) with a 6 s supervision timeout.

## Libraries

Install these from the Arduino Library Manager. The versions below are the ones installed on the machine used to compile this sketch. That compile is not a hardware test.

| Library | Version used |
|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.x | 2.5.2 |
| Adafruit SSD1306 | 2.5.17 |
| Adafruit GFX Library | 1.12.6 |

NimBLE-Arduino 2.x is required. The sketch uses the 2.x client, server, scan, and advertising calls (`setConnectionParams`, `advertiseOnDisconnect`, `getPeerDevices` is not required).

The ESP32-WROOM controller in this core accepts 3 BLE links (`CONFIG_BTDM_CTRL_BLE_MAX_CONN`). A higher `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is rejected. The C3 controller has room for 3 links plus scan and advertising. The sketch's own limit is `MAX_LINKS` 3. A local edit in the installed NimBLE `nimconfig.h` forces that library value to 3 so a later library default cannot raise it on the WROOM. That file is outside this repository. If you install NimBLE fresh, keep the connection count at 3 on the WROOM.

## Arduino IDE settings

1. Board: **ESP32 Dev Module** or **ESP32C3 Dev Module**.
2. Upload the sketch `LeMidiReci.ino`. The folder name must stay `LeMidiReci`.
3. On the C3, set **USB CDC On Boot** to **Enabled**. The ROM USB port is not the hardware MIDI UART.
4. If the WROOM boot log prints `flash read err, 1000`, set **Flash Frequency** to **40 MHz** and upload again. This was seen on one board and is not fully diagnosed.
5. USB serial monitors use **115200** baud. The MIDI UART does not. Do not point a serial monitor at the MIDI TX pin.

## Installation

1. Install the libraries above.
2. Open `LeMidiReci/LeMidiReci.ino`.
3. Select the board and, on the C3, USB CDC On Boot.
4. Upload.

No Python package or extra source file is required to build the firmware. `test/test_midi_ble.cpp` is a host test for the parser. The Arduino build does not compile that directory.

## Pinout

| Function | ESP32-WROOM-32 | ESP32-C3 | Notes |
|---|---|---|---|
| USB serial TX/RX | GPIO 1 / GPIO 3 | native USB GPIO 19 / GPIO 18 | Do not reuse these for MIDI |
| OLED SDA / SCL | GPIO 5 / GPIO 4 | GPIO 5 / GPIO 4 | I²C `0x3C` |
| MIDI TX / RX | GPIO 17 / GPIO 16 | GPIO 7 / GPIO 6 | Change `MIDI_UART_TX` and `MIDI_UART_RX` together |
| Activity LED | off unless configured | GPIO 8 if enabled | Active low by default |
| Boot | GPIO 0 | GPIO 9, also GPIO 2 and GPIO 8 are strapping pins | MIDI TX is rejected at compile time if it hits these |

GPIO 8 on the C3 is a strapping pin. Define `LED_BUILTIN` only when that pin really drives an LED.

## USB serial modes

```cpp
#define SERIAL_MODE 2
```

| Mode | USB serial at 115200 |
|---|---|
| 0 | The sketch writes nothing. `Serial.begin` is not called |
| 1 | Remapped MIDI bytes only. No text from the sketch, no BLE timestamps |
| 2 | Connection text, remapped messages, and a stats line |

Mode 1 and mode 2 never share the USB port. The hardware UART still runs in every mode when `ENABLE_UART_MIDI` is 1.

On the WROOM-32 the CP2102 adapter prints the ROM boot log at 115200 before `setup()` runs. Mode 1 cannot suppress that. After the sketch starts, the application writes only MIDI bytes. On the C3 the USB CDC port does not carry that ROM UART log. Either way, a COM port full of MIDI bytes is not a USB-MIDI device.

Mode 2 example, Pocket remapped from channel 1 to channel 2, then a tune request:

```text
MIDI 1/3 connected: 93:cd:f1:79:5c:9a CH2
MIDI notify on: 93:cd:f1:79:5c:9a
IN 93:cd:f1:79:5c:9a CH1->CH2 90 30 52 -> 91 30 52 t128
IN 93:cd:f1:79:5c:9a F6 t140
stats msg=2 err=0 syxOv=0 wrap=0 clock=0 sense=0 uartDrop=0 usbDrop=0 dbgDrop=0
```

`t128` is the 13-bit BLE-MIDI timestamp, not a MIDI time-code byte. MIDI clock (`F8`) and active sensing (`FE`) are counted in `clock` and `sense` and are not printed one by one, because a clock stream would fill the debug port. They are still written to the UART and to USB mode 1. Start, stop, and continue are printed (`RT FA`, `RT FB`, `RT FC`).

A two-byte program change and a three-byte note look like this when the channel changes:

```text
IN aa:bb:cc:dd:ee:ff CH1->CH3 C0 05 -> C2 05 t20
IN aa:bb:cc:dd:ee:ff CH1->CH3 90 24 46 -> 92 24 46 t21
```

Mode 1 writes those output bytes with no spaces and no line breaks: `C2 05` then `92 24 46` as the six raw bytes `C2 05 92 24 46`.

## Hardware UART MIDI

```cpp
#define ENABLE_UART_MIDI 1
#define MIDI_BAUD 31250
```

The UART is 8N1. It carries the same remapped MIDI bytes as USB mode 1, including clock, active sensing, system common, and SysEx up to 128 bytes. BLE timestamps are not written. Debug text is not written.

The byte queue is 256 bytes. A message is copied whole or not at all. If it does not fit, `uartDrop` increments and that message is discarded. The BLE callback does not wait for the UART. `loop()` writes only while `availableForWrite()` has space.

Set `ENABLE_UART_MIDI` to 0 to leave the UART and its pins alone. USB debug can stay on while the UART is on. That is the normal way to watch connections and still feed a MIDI port.

## MIDI channel map

```cpp
static const DeviceChannel kDeviceChannels[] = {
    {"SMC-PAD Pocket", 2},
    {"SMC-PAD", 3},
};
```

`id` is either the advertised name or the BLE address `aa:bb:cc:dd:ee:ff`. Matching is exact and case-insensitive, so `SMC-PAD` does not match `SMC-PAD Pocket`. Address entries are checked first. The first match wins. Anything else stays on channel 1. A table entry outside 1–16 is treated as channel 1.

Names are learned from advertisements while scanning. A host that connects inward, and was never scanned, is recognized by address only.

Channel voice (note, poly pressure, control change, program change, channel pressure, pitch bend) is rewritten. Note on with velocity 0 is forwarded unchanged, not converted to note off. System common, system real-time, and SysEx keep their bytes. Two devices may share a target channel. The link is still tracked by BLE address.

Running status inside one BLE packet is restored before remap, so the channel nibble is still rewritten. The next packet starts with no running status.

## BLE-MIDI central

The sketch scans for service `03B80E5A-EDE8-4B33-A751-6CE34EC4C700` and subscribes to characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3`.

It connects to one new peripheral at a time, then keeps scanning until three links are in use or the scan finds nothing new. An address that is already connected is ignored. After a disconnect or a failed connect it waits 500 ms and scans again. A device that is off or out of range is simply absent until it advertises again. There is no stored reconnect list beyond "scan for the MIDI service".

Disconnect reason **520** is NimBLE's `0x200 + 0x08`: the supervision timeout expired. On this firmware that showed up when advertising or a 100% scan window ran beside an open central link. The current code stops advertising while a central link exists and shortens the scan window. It does not change the connection interval as a blanket response. If 520 remains, the peer, the distance, and 2.4 GHz interference are still possible causes. This was not re-measured on hardware for this revision.

Failed connects release the NimBLE client back to the disconnected-client pool (`getDisconnectedClient`). The per-link parser is cleared on disconnect, including a SysEx that was cut off.

## BLE-MIDI peripheral

With no outgoing central connection, the ESP32 advertises the MIDI service and the name **LeMidiReci**. A computer or phone can connect and write MIDI to the same characteristic. Those peers use the same pool of three links. Incoming writers are parsed and remapped like pads. Their channel is 1 unless their address is in the table.

`advertiseOnDisconnect(false)` is set so NimBLE does not restart advertising on its own. `refreshAdvertising()` is the only place that starts it, and only when `outgoingCount()` is 0 and a free link remains.

## OLED

```cpp
#define USE_DISPLAY 0
```

Set this to `1` only when the SSD1306 is wired. At `0` the display libraries are not used and no I²C pins are claimed by the sketch.

With the display on, eight text rows are used. The first row is the link count. Up to three rows name the connected devices and their target channel. The remaining rows are the latest MIDI messages, newest at the bottom. A remapped note shows the original bytes, then the bytes that were forwarded:

```text
BLE 2/3
Pocket     CH02
SMC-PAD    CH03
90 24 46>91 24 46
B0 07 7F>B2 07 7F
```

`BLE 1/3 r520` means the last disconnect reason is still 520. SysEx is shown as `SYX` plus the byte count, not as a truncated three-byte line. Clock and active sensing are omitted so they do not fill the screen. The screen is redrawn only when the text changes, and MIDI-only updates wait at least 80 ms. I²C is set to 400 kHz. The redraw still blocks `loop()` for one full display transfer. It does not run inside the BLE callback.

## Activity LED

```cpp
#define LED_BUILTIN 8
#define LED_ACTIVE_LOW 1
```

Comment out `LED_BUILTIN` and the pin is never configured. The LED turns on for about 30 ms when a MIDI message is forwarded, using `millis()` rather than `delay()`. Clock and active sensing do not light it, or a 24 Hz clock would hold it on. `LED_ACTIVE_LOW 0` lights the LED when the pin is high.

## Configuration

All of these are at the top of `LeMidiReci.ino`.

| Define | Default in this tree | Meaning |
|---|---|---|
| `BOARD_ESP32C3` | commented out | `0` WROOM-32, `1` C3. Omit it to follow the IDE |
| `SERIAL_MODE` | `2` | `0` silent, `1` USB MIDI bytes, `2` USB debug text |
| `USE_DISPLAY` | `0` | `1` enables the SSD1306 |
| `LED_BUILTIN` | `8` | GPIO of the activity LED. Comment out to disable |
| `LED_ACTIVE_LOW` | `1` | `1` on when low, `0` on when high |
| `ENABLE_UART_MIDI` | `1` | `1` enables the 31250 baud UART |
| `MIDI_BAUD` | `31250` | Hardware MIDI baud rate |
| `MIDI_UART_TX` / `MIDI_UART_RX` | 17/16 or 7/6 | UART pins. RX is unused for input |
| `kDeviceChannels` | Pocket 2, SMC-PAD 3 | Name or address to MIDI channel |

`MAX_LINKS` is 3 because the WROOM controller is 3. Do not raise it without replacing the controller limit. `MIDI_SYSEX_MAX` in `midi_ble.h` is 128 bytes including `F0` and `F7`. A longer SysEx is dropped, counted in `syxOv`, and the parser waits for `F7` before accepting new SysEx.

## Troubleshooting

| What you see | What it means |
|---|---|
| Garbage after a readable `ets Jun 8 2016` boot line | The monitor baud is not 115200, or mode 1 is printing raw MIDI into a text monitor |
| `flash read err, 1000` | Try Flash Frequency 40 MHz. Not confirmed fixed on every board |
| Connects, then `reason 520` | Supervision timeout. Confirm advertising is off while a pad is connected. Reduce distance and 2.4 GHz load. The 500 ms rescan is only a pause, not a radio fix |
| Only one pad stays connected | The link budget is three, but a second connection needs the short scan window. If 520 returns, the radio is still dropping the first link |
| Mode 1 looks like text garbage | That is the MIDI byte stream. Use a serial-to-MIDI bridge, or switch to mode 2 to read it |
| DIN device stays silent | GPIO TX is not a DIN driver. Check the output circuit, the baud (31250, not 115200), and `ENABLE_UART_MIDI` |
| OLED stays blank | `USE_DISPLAY` is 0 in this tree. Set it to 1. Address `0x3C`, SDA 5, SCL 4 |
| C3 prints nothing | Enable USB CDC On Boot and wait for the port. Mode 0 calls no `Serial.begin` |
| Unknown pad is on channel 1 | Add its address or its exact advertised name. Inbound hosts are matched by address unless they were scanned |

## Known limitations

- Three BLE links total, shared by central and peripheral roles.
- No MIDI IN. The UART RX pin is not parsed.
- No USB-MIDI class device. Mode 1 is raw bytes on a COM port.
- SysEx longer than 128 bytes is dropped.
- A non-SysEx message split across BLE packets is discarded. The specification does not split those messages. The error counter increments.
- Running status does not continue into the next BLE packet.
- Inside one packet, system common does not cancel running status. That follows BLE-MIDI, not the older DIN rule that system common cancels it.
- MIDI clock and active sensing are forwarded but not logged line by line and do not blink the LED.
- The OLED redraw blocks `loop()` for one I²C frame when the text changes.
- The WROOM ROM boot text still appears on UART0 before mode 1 starts.
- BLE timestamps are diagnostic only. They are not used to schedule the UART.
- Parser slots and the display list hold three peers. A fourth packet in one callback would be counted as an error. The controller should not open a fourth link.
- Color and configuration writes to an SMC-PAD are not implemented. See below.

## Tests

Host tests in `test/test_midi_ble.cpp` cover the parser and the output queues. They compile as C against `midi_ble.h` and do not need a board. On the machine used for this revision they were compiled with TinyCC 0.9.27 and the result was `ok`. They are not an on-device test.

The firmware was compiled with arduino-esp32 3.3.11 and NimBLE-Arduino 2.5.2. That is a compile, not a hardware test:

| Build | Result |
|---|---|
| ESP32 Dev Module, sketch as in the tree (`USE_DISPLAY 0`) | success, 610724 bytes flash (46%), 40148 bytes RAM (12%) |
| ESP32C3 Dev Module, USB CDC On Boot, same sketch | success, 618439 bytes flash (47%), 27272 bytes RAM (8%) |
| ESP32 Dev Module, a copy with `USE_DISPLAY 1` | success, 647940 bytes flash (49%), 41972 bytes RAM (12%) |
| ESP32C3 Dev Module, USB CDC On Boot, a copy with `USE_DISPLAY 1` | success, 658317 bytes flash (50%), 28360 bytes RAM (8%) |

The two display builds used a temporary copy. The sketch in this repository still has `USE_DISPLAY 0`.

Covered there:

- Note on, note off, and note on with velocity 0 left as a note on.
- Program change and channel pressure (two-byte messages) and pitch bend (three bytes).
- Running status, and running status cancelled at the next packet.
- Several messages in one BLE packet.
- SysEx split across two packets, and a clock byte inside a SysEx.
- Real-time bytes between the data bytes of a note, plus start, stop, continue, and active sensing.
- A 13-bit timestamp wrap inside a packet and across two packets.
- A bad header, a truncated note, undefined `F4`/`F5`, and a later valid note in the same packet.
- SysEx overflow past 128 bytes, then a following note.
- Target channels 1 through 16, and an out-of-range target forced to channel 1.
- Two parsers at once: one held in SysEx while the other emits a note.
- UART and USB queues enabled together, one at a time, or both off, including a queue that refuses a partial message.

Not done:

- No logic analyzer capture of the 31250 baud UART.
- No BLE connection with an SMC-PAD, a phone, or a second pad for this revision.
- No OLED photo and no LED check.
- No confirmation that reason 520 is gone on the bench.
- Firmware compilation is a separate step from the host tests. See the build log for the board you upload. Do not treat a successful compile as a hardware test.

To repeat the host test, compile `test/test_midi_ble.cpp` as C99 with `midi_ble.h` on the include path. The Arduino IDE will not do that.

## SMC-PAD RGB (not implemented)

The pads do not light from MIDI. Public reverse engineering treats color as a write into the pad's settings memory, on GATT service `AE40`, write characteristic `AE41`, notify `AE42`:

- [spectalive/smc-pad](https://github.com/spectalive/smc-pad) documents the larger SMC-PAD. A color write is ignored until the client has replayed a long unlock that reads the whole config. The same session is what the official editor uses to store a preset. Those writes target flash addresses in that project's notes, so they can change the saved configuration, not only a temporary lamp.
- [SageSchiller/thump](https://github.com/SageSchiller/thump) says the SMC-PAD Pocket speaks the same protocol, and that its own colors return when the app closes or the pad loses power. It also describes four presets selected by holding a pad at power-on. That is a different memory layout from a single 16-pad bank even if the packet framing matches.

Those two reports do not agree about whether a color write is still there after power-off. This firmware has not spoken to either pad's config service, so LeMidiReci does not know which behavior a given unit has. Do not send `AE41` writes from this sketch until someone checks, on that exact model and firmware:

1. Whether the unit is an SMC-PAD or an SMC-PAD Pocket, and which preset and bank it is in.
2. Whether the unlock read must finish before any write, and how long that read takes on the same radio that is carrying MIDI.
3. Whether a write commits to flash. Read the bytes back, power-cycle, and read them again before assuming they were temporary.
4. Whether the Pocket's preset block and the larger pad's address map (`0x418 + (note - 36) * 26` in the spectalive notes) are the same on the unit you have.

A future option should be off by default, should not run the unlock during the MIDI connection setup, and should not advertise or scan at full duty while it runs. Until those checks exist, LeMidiReci only receives MIDI.

## License

LeMidiReci is free software, released under the [GNU General Public License v3.0](LICENSE) or, at your option, any later version.

Copyright (C) 2026 RichardSmetana. There is no separate firmware version number. The git revision identifies a build.
