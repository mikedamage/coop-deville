# LoRa Polling Protocol Specification

## Network Topology

**Star topology** with a single gateway polling up to 10 remote nodes in round-robin fashion over LoRa (SX1262, 915 MHz ISM band).

```
                    ┌─────────────┐
              ┌────▶│ Remote Node │ (0x02)
              │     │  (sensor)   │
              │     └─────────────┘
┌─────────┐   │     ┌─────────────┐
│ Gateway │◀──┼────▶│ Remote Node │ (0x03)
│  (0x01) │   │     │  (sensor)   │
└─────────┘   │     └─────────────┘
   │ WiFi     │     ┌─────────────┐
   ▼          └────▶│ Remote Node │ (0x04)
Home Assistant       │  (sensor)   │
                     └─────────────┘
```

- **Gateway**: Connects to WiFi and Home Assistant. Initiates all communication. Polls each remote node in a fixed time slot, forwards sensor data to HA as virtual devices.
- **Remote nodes**: Battery/solar powered. No WiFi dependency. Respond only when polled. Support scheduled listen windows to conserve power.

## Addressing

| Address | Usage |
|---------|-------|
| `0x01`–`0xFE` | Assignable node addresses (254 usable) |
| `0xFF` | Broadcast (all nodes listen) |
| `0x00` | Reserved / invalid |

Addresses are manually assigned. The user is responsible for uniqueness.

## Byte Order

All multi-byte integers use **little-endian** byte order.

## Authentication

All packets are authenticated using **SipHash-2-4** with a mandatory 128-bit (16-byte) pre-shared key configured identically on the gateway and all remote nodes.

### Auth Footer (4 bytes, appended to every packet)

```
Byte N-3:  Sequence number [7:0]
Byte N-2:  Sequence number [15:8]   (uint16_t LE)
Byte N-1:  Auth tag [7:0]
Byte N:    Auth tag [15:8]          (uint16_t LE)
```

**Signing procedure:**
1. Construct packet body (command-specific bytes)
2. Append 2-byte sequence number (little-endian), increment sender's counter
3. Compute SipHash-2-4 over (body + seq) using the pre-shared key
4. Truncate the 64-bit hash to 16 bits (`hash & 0xFFFF`)
5. Append 2-byte auth tag (little-endian)

**Verification procedure:**
1. Check packet size >= AUTH_OVERHEAD (4 bytes)
2. Split packet into `body_plus_seq` (all but last 2 bytes) and `tag` (last 2 bytes)
3. Compute SipHash-2-4 over `body_plus_seq`, compare truncated result to received tag
4. Extract sequence number from the 2 bytes before the tag
5. Validate sequence against the sliding window

### Anti-Replay Protection

Each sender maintains a monotonically increasing 16-bit sequence number (wraps at 65535). Each receiver tracks the last accepted sequence per sender using a **sliding window of 256**:

- A packet is accepted if `seq` is within `[last_seq + 1, last_seq + 256]` (modular uint16_t arithmetic)
- The first packet from a sender is always accepted and initializes the window
- The gateway tracks sequences **per remote node**; remote nodes track a single gateway sequence

### Key Configuration

The `auth_key` config option accepts either format:
- **Hex string**: 32 hex characters (e.g., `"0102030405060708090a0b0c0d0e0f10"`)
- **Base64**: base64-encoded 16-byte key (e.g., `"AQIDBA..."`)

## Packet Formats

### Common Header (3 bytes)

All packets begin with:

```
Byte 0:  Source address      (uint8_t)
Byte 1:  Destination address (uint8_t)
Byte 2:  Command             (uint8_t)
```

### Commands

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| `0x01` | `CMD_POLL_REQUEST` | Gateway → Node | Request sensor data |
| `0x02` | `CMD_POLL_RESPONSE` | Node → Gateway | Sensor data response |
| `0x03` | `CMD_TIME_SYNC` | Gateway → Broadcast | Time and schedule sync |
| `0x04` | `CMD_ACK` | Gateway → Node | Acknowledge receipt (optional) |

### Poll Request (7 bytes)

```
Byte 0:    Gateway address
Byte 1:    Target node address
Byte 2:    0x01 (CMD_POLL_REQUEST)
Bytes 3-6: Auth footer
```

### Poll Response (9+ bytes)

```
Byte 0:    Source node address
Byte 1:    Gateway address
Byte 2:    0x02 (CMD_POLL_RESPONSE)
Byte 3:    Packet number (0x00 = single, 0x01+ = multi-packet fragment)
Byte 4:    Total packets (0x01 = single, 0x02+ = multi-packet)
Bytes 5+:  Sensor data payload
Last 4:    Auth footer
```

Maximum payload per packet: **246 bytes** (255 max - 5 header - 4 auth).

### Time Sync Broadcast (18 + N bytes)

Broadcast to `0xFF`. Includes schedule information for listen window computation.

```
Byte 0:      Gateway address
Byte 1:      0xFF (broadcast)
Byte 2:      0x03 (CMD_TIME_SYNC)
Bytes 3-6:   Unix timestamp, seconds since epoch (uint32_t LE)
Bytes 7-10:  Poll interval in ms (uint32_t LE)
Bytes 11-12: Slot duration in ms (uint16_t LE)
Byte 13:     Node count (uint8_t)
Bytes 14+:   Node addresses in poll order (node_count bytes)
Last 4:      Auth footer
```

### ACK (7 bytes)

Optional. Sent after receiving a complete poll response.

```
Byte 0:    Gateway address
Byte 1:    Target node address
Byte 2:    0x04 (CMD_ACK)
Bytes 3-6: Auth footer
```

## Sensor Data Payload Format

Sensor data is serialized as a sequence of tagged values:

### Float Sensor

```
Byte 0:       0x01 (SENSOR_KEY)
Bytes 1-4:    Float value (IEEE 754, 4 bytes)
Byte 5:       Name length (uint8_t)
Bytes 6+:     Name string (UTF-8, length bytes)
```

### Binary Sensor

```
Byte 0:       0x02 (BINARY_SENSOR_KEY)
Byte 1:       Boolean value (0x00 = false, 0x01 = true)
Byte 2:       Name length (uint8_t)
Bytes 3+:     Name string (UTF-8, length bytes)
```

### Multi-Packet Fragmentation

If serialized sensor data exceeds 246 bytes, it is split across multiple response packets:
- Fragment 1: `packet_num=0x01`, `total_packets=N`
- Fragment 2: `packet_num=0x02`, `total_packets=N`
- ...
- Single-packet responses use `packet_num=0x00`, `total_packets=0x01`
- 15 ms delay between fragment transmissions for gateway RX turnaround

The gateway reassembles fragments by source address, concatenating payloads in order. Incomplete assemblies are discarded on timeout.

## Polling Timing

### Fixed Time-Slot Polling

The gateway uses fixed time slots to ensure predictable poll timing:

```
slot_duration = response_timeout + 200ms (SLOT_MARGIN_MS)
cycle_duration = slot_duration * node_count
```

Within each cycle:
```
Time:   |-- slot 0 --|-- slot 1 --|-- slot 2 --|--- idle ---|
        |  poll N0   |  poll N1   |  poll N2   |  (wait)    |
        |<- slot_d ->|<- slot_d ->|<- slot_d ->|            |
        |<-------------- poll_interval ---------------------->|
```

- The gateway validates at startup that `poll_interval >= cycle_duration`
- Each node is polled at a deterministic offset: `cycle_start + index * slot_duration`
- Time sync is broadcast at the start of a new cycle if the sync interval has elapsed

### Poll Interval vs Cycle Duration

- `poll_interval`: time between the start of successive polling cycles (configurable)
- `cycle_duration`: minimum time to poll all nodes once (computed, not configurable)
- If `poll_interval < cycle_duration`, it is automatically increased to `cycle_duration` with a warning

## Listen Windows (Remote Node Power Saving)

Remote nodes can duty-cycle their radio to reduce power consumption. When enabled, the radio sleeps in **standby RC mode** (~0.6 µA) between poll slots and wakes only for a narrow window around the expected poll time.

### State Machine

```
                        ┌──────────────────┐
        power on ──────▶│  Continuous RX   │◀──── 5 missed polls
                        │  (awaiting       │      (fallback)
                        │   schedule)      │
                        └───────┬──────────┘
                                │ first time sync
                                │ with schedule
                                ▼
                        ┌──────────────────┐
              ┌────────▶│  Radio Standby   │
              │         │  (sleeping)      │
              │         └───────┬──────────┘
              │                 │ next_listen_start reached
              │                 ▼
              │         ┌──────────────────┐
              │         │  Radio RX        │
              │         │  (listen window) │
              │         └──┬──────────┬────┘
              │            │          │
              │     poll received     │ window expired
              │            │          │ (no poll)
              │            ▼          ▼
              │      ┌──────────┐  missed++
              │      │ Respond  │  if missed >= 5:
              │      │ (TX)     │    fallback
              │      └────┬─────┘  else:
              │           │          sleep + recompute
              └───────────┘
```

### Window Timing

The listen window is centered on the predicted poll arrival time:

```
next_poll_time = last_poll_received + poll_interval
window_start   = next_poll_time - guard_window / 2
window_end     = next_poll_time + guard_window / 2
```

**Guard window derivation** (default: 50 ms):

| Source | Contribution | Notes |
|--------|-------------|-------|
| DS3231 RTC drift | ±0.12 ms / 60s | ±2 ppm at 0–40°C, negligible |
| ESP32 crystal variance | ±1.2 ms / 60s | Negligible |
| ESPHome `loop()` jitter | ±16 ms | **Dominant factor** |
| SX1262 standby→RX | ~0.5 ms | STDBY_RC wake time |
| Safety padding | 10 ms | Account for random variance |
| **Total** | **~50 ms** | Configurable via `listen_window` |

### Schedule Acquisition

1. Remote node powers on in **continuous RX**
2. Gateway broadcasts time sync with schedule (poll_interval, slot_duration, node list)
3. Remote node finds its `slot_index` in the address list
4. First poll expected at `(slot_index + 1) * slot_duration` after time sync
5. Radio transitions to windowed mode: sleep → wake at window_start → listen → respond → sleep
6. Timing re-anchors on each successful poll reception (not just time syncs)

### Fallback

After **5 consecutive missed polls** (`MAX_MISSED_POLLS`), the node falls back to continuous RX to re-acquire the schedule. This handles:
- Gateway restarts / schedule changes
- Significant clock drift accumulation
- Temporary RF interference

## Configuration Reference

### Gateway (`lora_gateway`)

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `sx126x_id` | ID | Yes | — | SX126x radio component |
| `address` | hex uint8 | Yes | — | Gateway address (0x01–0xFE) |
| `auth_key` | string | Yes | — | 16-byte key (hex or base64) |
| `response_timeout` | time | Yes | — | Max wait for poll response |
| `poll_interval` | time | Yes | — | Time between polling cycles |
| `time_id` | ID | No | — | RealTimeClock for time sync |
| `time_sync_interval` | time | No | — | Interval between time sync broadcasts |
| `send_ack` | bool | No | `false` | Send ACK after receiving response |
| `stale_sensor_behavior` | enum | No | `keep` | `keep` or `invalidate` on timeout |
| `remote_nodes` | list | Yes | — | List of remote node definitions |

### Remote Node (`lora_remote_node`)

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `sx126x_id` | ID | Yes | — | SX126x radio component |
| `address` | hex uint8 | Yes | — | Node address (0x01–0xFE) |
| `auth_key` | string | Yes | — | 16-byte key (hex or base64) |
| `time_id` | ID | No | — | RealTimeClock for time sync |
| `listen_window` | time | No | disabled | Guard window duration (enables power saving) |
| `sensors` | list | No | `[]` | Sensor IDs to report |
| `binary_sensors` | list | No | `[]` | Binary sensor IDs to report |

## Protocol Constants

```
AUTH_KEY_SIZE          = 16      bytes
AUTH_TAG_SIZE          = 2       bytes
SEQ_NUM_SIZE           = 2       bytes
AUTH_OVERHEAD          = 4       bytes
SEQ_WINDOW_SIZE        = 256
MAX_PACKET_SIZE        = 255     bytes
MAX_PAYLOAD_SIZE       = 246     bytes
POLL_REQUEST_SIZE      = 7       bytes
ACK_PACKET_SIZE        = 7       bytes
TIME_SYNC_HEADER_SIZE  = 14      bytes (before node list)
SLOT_MARGIN_MS         = 200     ms
DEFAULT_GUARD_WINDOW   = 50      ms
MAX_MISSED_POLLS       = 5
```
