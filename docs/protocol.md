# getAir SmartFan / Viessmann Vitovent 100-D RS485 Protocol

This document describes the reverse-engineered communication protocol used by the getAir SmartFan decentralized ventilation system and the rebranded Viessmann Vitovent 100-D.

The protocol was captured from both the original LED Control Unit and the Smart Control Unit. Some behavior is directly observed on the bus, while a few details are still inferred and marked accordingly.

---

# 1. System Overview

- Physical layer: RS485, half duplex
- Topology: multi-drop bus
- One master controller
- Up to 6 fan nodes with addresses `0x00`–`0x05`
- UART parameters: `2400 baud, 8N1`

The protocol itself is effectively stateless:

> The master must continuously repeat the desired state for every active node.

If the master stops sending frames, the fans eventually stop updating.

---

# 2. Physical Layer

The bus uses a standard RS485 transceiver.

Typical transceiver signals:

- `DI` = UART TX to bus
- `RO` = UART RX from bus
- `DE` = driver enable
- `/RE` = receiver enable

The bus is half-duplex, therefore only one participant may transmit at a time.

For ESP32 implementations, `DE` and `/RE` are usually tied together and controlled by one GPIO.

Observed hardware:

- PIC16F1509 microcontroller
- ST3485 RS485 transceiver

---

# 3. Node Addressing

Each fan has a hardware-configured address selected via DIP switches.

Observed valid node addresses:

| Node | Address Byte |
|------|------|
| Fan 0 | `0x00` |
| Fan 1 | `0x01` |
| Fan 2 | `0x02` |
| Fan 3 | `0x03` |
| Fan 4 | `0x04` |
| Fan 5 | `0x05` |

The original controller periodically scans all possible node addresses.

---

# 4. Frame Structure

All observed frames begin with a 3-byte header:

| Direction | Header |
|------|------|
| Master → Fan | `55 4D 00` |
| Fan → Master | `55 53 00` |

The last byte before the optional terminator is always the checksum.

Some controller variants append an additional `00` terminator byte.

Observed behavior:

- LED Control Unit often omits the final `00`
- Smart Control Unit frequently appends the final `00`
- Fan replies may also optionally end with `00`

---

# 5. Command Frame

This is the main control frame sent by the master.

```text
55 4D 00 TT 03 NN DD SS CC [00]
```

| Byte | Meaning |
|------|------|
| 0–2 | Header `55 4D 00` |
| 3 | Telemetry request byte |
| 4 | Frame type, always `03` |
| 5 | Node address |
| 6 | Direction |
| 7 | Speed |
| 8 | Checksum |
| 9 | Optional terminator `00` |

Example:

```text
55 4D 00 96 03 01 01 2F E9 00
```

This means:

- no sensor request
- node `0x01`
- exhaust direction
- speed `0x2F` = 47 %

## 5.1 Telemetry Request Byte

| Value | Meaning |
|------|------|
| `0x96` | Normal command, no sensor request |
| `0x97` | Request sensor data from the addressed fan |

`0x97` was observed only when the original system had optional humidity/temperature sensors installed.

Sensor polling does not need to happen continuously. Polling every few seconds is sufficient because humidity and temperature change slowly.

## 5.2 Direction Byte

| Value | Meaning |
|------|------|
| `0x01` | Exhaust air |
| `0x02` | Supply air |

## 5.3 Speed Byte

The speed value appears to behave like a PWM duty cycle.

Observed range:

| Value | Meaning |
|------|------|
| `0x00` | Off |
| `0x14` | Minimum practical speed (~20 %) |
| `0x64` | 100 % |

Typical values used by the original controller are between `0x14` and `0x64`.

---

# 6. Presence Poll Frame

The Smart Control Unit sends an additional frame that appears to be used for node discovery.

```text
55 4D 00 6E 01 NN CC [00]
```

| Byte | Meaning |
|------|------|
| 0–2 | Header `55 4D 00` |
| 3 | `6E` |
| 4 | `01` |
| 5 | Node address |
| 6 | Checksum |
| 7 | Optional terminator `00` |

Observed behavior:

- Sent cyclically to all possible node addresses
- Typically used for inactive or currently missing nodes
- Not observed on the older LED Control Unit
- Ignoring this frame in a custom ESP32 master still allows normal operation

Therefore the exact purpose is still inferred, but most likely:

> Presence detection / node discovery

---

# 7. Sensor Reply Frame

When a node is polled using telemetry byte `0x97`, it may answer with the following frame:

```text
55 53 00 98 05 NN DD HH TT TT CC [00]
```

| Byte | Meaning |
|------|------|
| 0–2 | Header `55 53 00` |
| 3 | `98` |
| 4 | `05` |
| 5 | Node address |
| 6 | Current direction |
| 7 | Humidity |
| 8–9 | Temperature, 16-bit big-endian |
| 10 | Checksum |
| 11 | Optional terminator `00` |

Example:

```text
55 53 00 98 05 02 01 2F 00 EE D2
```

Decoded:

- node `0x02`
- exhaust direction
- humidity `0x2F` = 47 %
- temperature `0x00EE` = 238 → 23.8 °C

## 7.1 Direction in Reply

| Value | Meaning |
|------|------|
| `0x01` | Exhaust |
| `0x02` | Supply |

## 7.2 Humidity

Humidity is transmitted directly as percent.

```text
humidity = byte7
```

Example:

```text
0x2F = 47 %
```

## 7.3 Temperature

Temperature is encoded as a 16-bit unsigned integer in big-endian format.

```text
temperature = ((byte8 << 8) | byte9) / 10.0
```

Example:

```text
00 EE = 238 = 23.8 °C
```

Observed update interval:

- Approximately every 10 seconds
- Some replies are asynchronous and not strictly tied to every command frame

---

# 8. Presence / ACK Reply Frame

Fans may also respond to the discovery frame with:

```text
55 53 00 6E 02 XX NN CC [00]
```

| Byte | Meaning |
|------|------|
| 0–2 | Header `55 53 00` |
| 3 | `6E` |
| 4 | `02` |
| 5 | Unknown |
| 6 | Node address |
| 7 | Checksum |
| 8 | Optional terminator `00` |

The purpose of byte 5 is currently unknown.

Most likely interpretation:

> Alive / presence acknowledgement

---

# 9. Checksum

All observed frames use the same additive 8-bit checksum.

```text
checksum = (0x55 - (sum(frame_without_checksum) & 0xFF)) & 0xFF
```

Equivalent rule:

```text
(sum(all_bytes_including_checksum) & 0xFF) == 0x55
```

The optional final `00` terminator is not part of the checksum calculation.

---

# 10. Observed System Behavior

## 10.1 Active Nodes

A node is considered active once it replies to a poll frame.

Observed controller behavior:

- Active nodes receive command frames continuously
- Missing nodes receive only discovery frames
- The controller periodically retries missing nodes

## 10.2 Heat Recovery Mode

The original system periodically swaps airflow direction.

Observed timing:

- approximately every 50–70 seconds
- paired fans alternate between supply and exhaust

Example:

| Fan A | Fan B |
|------|------|
| Supply | Exhaust |
| Exhaust | Supply |

## 10.3 Sensor Interpretation

Only one physical sensor appears to exist per fan.

The original software interprets the same sensor differently depending on current airflow direction:

| Fan Direction | Interpreted As |
|------|------|
| Supply | Outdoor temperature / humidity |
| Exhaust | Indoor temperature / humidity |

---

# 11. Notes for Custom Controllers

A custom master implementation such as an ESP32 gateway only needs:

1. Node discovery
2. Periodic command transmission
3. Optional sensor polling
4. Internal state tracking per node

Recommended command repetition interval:

- approximately once per second per active node

Recommended software state per node:

- online / offline
- speed
- direction
- last humidity
- last temperature
- timestamp of last reply

---

# 12. Open Questions

The following protocol details are still not fully understood:

- Exact purpose of byte 5 in `6E 02` reply frames
- Whether additional frame types exist
- Whether newer controller revisions use different timing or extra bytes
- Whether command byte `03` has additional variants

Contributions with further captures are welcome.

