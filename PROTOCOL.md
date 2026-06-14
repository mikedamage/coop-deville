# LoRa Polling Protocol Specification

## Network Topology

**Star topology** with a single gateway polling remote nodes in round-robin fashion over LoRa (SX1262, 915 MHz ISM band). The practical node count is bounded by `poll_interval ≥ slot_duration × N` (see *Polling Timing*).

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

### Gateway Sequence Persistence

The gateway persists its outbound `tx_seq` high-water mark to NVS in chunks of `TX_SEQ_RESERVE_CHUNK = 256`. Before using a seq value past the saved mark, the next chunk is reserved and flushed, so after an unclean reboot the gateway resumes at most 255 slots ahead of its last used value — never behind it. This prevents remote nodes from rejecting post-reboot packets as replays.

Remote nodes do **not** persist `tx_seq` — on reboot they restart from 0. The gateway's per-node rx window is in-RAM only, so a remote reboot is only transparently absorbed if the *gateway* has also rebooted since last contact (re-initializing the window on first packet). Otherwise, the remote's post-reboot packets will be rejected as out-of-window until its seq counter catches up to the gateway's recorded value. This is a known limitation; remote `tx_seq` persistence or a re-sync handshake may be added later.

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
| `0x01` | `CMD_POLL_REQUEST` | Gateway → Node | Request sensor data; optionally carries downlink commands |
| `0x02` | `CMD_POLL_RESPONSE` | Node → Gateway | Sensor data response; optionally carries command acks |
| `0x03` | `CMD_TIME_SYNC` | Gateway → Broadcast | Time and schedule sync (the only true broadcast frame) |
| `0x04` | `CMD_ACK` | Gateway → Node | Response-level acknowledgment of a complete poll response (optional, informational) |

> Note: `CMD_ACK` is distinct from the **command acks** that nodes return inside a poll response (see *Downlink Commands* below). `CMD_ACK` confirms "I received your response"; a command ack confirms "I processed the gateway's downlink command."

### Poll Request (7+ bytes, variable)

```
Byte 0:       Gateway address
Byte 1:       Target node address (unicast only — see below)
Byte 2:       0x01 (CMD_POLL_REQUEST)
Byte 3:       Command count (uint8_t; 0 = bare poll)
Bytes 4+:     Command envelopes (cmd_count × variable, see Downlink Commands)
Last 4:       Auth footer
```

When `cmd_count = 0`, no bytes follow before the auth footer and the frame is exactly 7 bytes — the legacy "bare poll" form. When `cmd_count ≥ 1`, one or more command envelopes are appended back-to-back; the remote walks them using each envelope's self-declared length.

**Poll requests are unicast only.** The target address must equal a node's own address. Remote nodes must reject poll requests sent to `0xFF`. (Prior revisions of this spec permitted broadcast polls; this is no longer allowed because simultaneous multi-node responses would collide, and commands are distributed per-node through the queue model rather than on the wire.)

### Poll Response (9+ bytes)

```
Byte 0:    Source node address
Byte 1:    Gateway address
Byte 2:    0x02 (CMD_POLL_RESPONSE)
Byte 3:    Packet number (0x00 = single, 0x01+ = multi-packet fragment)
Byte 4:    Total packets (0x01 = single, 0x02+ = multi-packet)
Bytes 5+:  Response payload (tagged values — sensor data, command acks, etc.)
Last 4:    Auth footer
```

Maximum payload per packet: **246 bytes** (255 max - 5 header - 4 auth). The payload is a sequence of tagged records (see *Sensor Data Payload Format*); a response to a poll that carried commands **must** begin with the corresponding command acks before any sensor data.

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

ACKs are authenticated (SipHash tag verified) but do **not** advance the remote's gateway sequence window. They are currently informational only; a replayed ACK causes no state change. If ACKs drive retry logic in the future, they must be brought under the replay window.

## Sensor Data Payload Format

Sensor data is serialized as a sequence of tagged values. The sensor's ESPHome `name` is transmitted on the wire as the identifier; the gateway resolves each incoming name by matching it against the `key` config field declared for that remote node. **The remote's sensor name must equal the gateway's `key` value** — otherwise the gateway logs an "Unknown sensor key" warning and drops the value.

Values are serialized as:

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

### Command Ack

Acknowledges processing of a downlink command received in the paired poll request. See *Downlink Commands* for the enqueue/delivery model.

```
Byte 0:       0x03 (COMMAND_ACK_KEY)
Bytes 1-2:    Command ID (uint16_t LE, echoes gateway's cmd_id)
Byte 3:       Result code (uint8_t — see Result Codes)
```

Every command delivered in a poll request **must** produce exactly one command ack in the paired poll response — even on failure. Acks for unknown `cmd_id` values are logged and ignored by the gateway. If all delivered commands are ack'd and there is no sensor data to report, the response payload consists solely of ack records.

**Result codes**:

| Value | Name | Meaning |
|-------|------|---------|
| `0x00` | `ACK_OK` | Command applied successfully |
| `0x01` | `ACK_UNKNOWN_OPCODE` | Opcode not recognized by this node |
| `0x02` | `ACK_UNKNOWN_TARGET` | Opcode valid but target entity not found |
| `0x03` | `ACK_INVALID_PAYLOAD` | Payload malformed or out of range |
| `0x04` | `ACK_REJECTED` | Node refused the command (e.g., safety interlock, entity in use) |

Any non-`ACK_OK` result is terminal for that command — the gateway removes it from the queue and does **not** retry (the node saw it and deterministically refused).

### Multi-Packet Fragmentation

If serialized sensor data exceeds 246 bytes, it is split across multiple response packets:
- Fragment 1: `packet_num=0x01`, `total_packets=N`
- Fragment 2: `packet_num=0x02`, `total_packets=N`
- ...
- Single-packet responses use `packet_num=0x00`, `total_packets=0x01`
- 15 ms delay between fragment transmissions for gateway RX turnaround

The gateway reassembles fragments by source address, concatenating payloads in order. Incomplete assemblies are discarded on timeout.

### Delta Compression

To reduce airtime, remote nodes only transmit sensor values that have **changed since the last poll response**. The gateway treats any sensors absent from a response as "unchanged" and keeps their last published state.

- The remote node caches the last-sent value for each sensor (raw float bytes for sensors, bool for binary sensors)
- On each poll, only sensors whose current value differs from the cached value are serialized
- If nothing changed, an empty payload is sent (the gateway still receives RSSI/SNR/liveness metrics)
- Every `full_update_interval` polls (default: 10), a **full update** is forced containing all sensor values regardless of change, to resync state after gateway restarts or missed responses
- The first poll is always a full update
- When a new gateway is detected (sequence number initialization), the next poll forces a full update

**Value comparison**:
- Float sensors: `memcmp` on the 4 raw IEEE 754 bytes (handles NaN, -0.0; ADC noise filtering should be done via ESPHome sensor `filters:`)
- Binary sensors: direct `==` comparison

**Configuration**: `full_update_interval` (optional, default 10, range 1-255). Setting to 1 effectively disables delta compression.

## Downlink Commands

The gateway can send commands to remote nodes (set a value, toggle a switch, invoke an action). Because remotes duty-cycle their radios and are only guaranteed to be listening around their own poll slot, commands are **piggybacked onto the poll request** for the target node. A node's next command-delivery opportunity is therefore bounded by one `poll_interval`.

There is **no broadcast frame** for commands. "Broadcast a command to all nodes" is modeled at the gateway as a fanout across per-node queues, not as a single radio transmission. Only time sync uses the `0xFF` address.

### Queue Model

The gateway maintains a **per-node FIFO command queue**. API callers submit commands to the gateway; the gateway routes each submission:

- **Unicast**: enqueued on the target node's queue only.
- **Broadcast**: enqueued on every registered node's queue as independent copies.

Queue properties:

| Property | Value / Behavior |
|----------|------------------|
| Per-node depth | `COMMAND_QUEUE_DEPTH = 8` |
| Overflow policy | Drop oldest (ring semantics) |
| Ordering | FIFO per node; no cross-node ordering guarantee |
| Coalescing | Per-opcode flag; see below |
| Persistence | None — queues live in RAM and are lost on gateway reboot |
| Drain rate | Up to `MAX_COMMANDS_PER_POLL` per poll, bounded also by packet size |

**Coalescing**: when a submitted command's `(opcode, target_name)` matches an entry already in the queue *and* the opcode is declared coalescing, the existing entry's payload is updated in place (queue position preserved). This keeps idempotent state-setters from stacking up behind a disconnected node — only the latest desired state survives. Non-coalescing opcodes always append.

### Command Envelope

Each command is framed as:

```
Byte 0:     Opcode         (uint8_t)
Bytes 1-2:  Command ID     (uint16_t LE, assigned by gateway)
Byte 3:     Payload length (uint8_t, N; may be 0)
Bytes 4+:   Payload        (N bytes, opcode-specific)
```

Envelopes are packed back-to-back after the `cmd_count` byte in a poll request. The remote parses envelopes sequentially using each declared payload length. If a declared length would overrun the packet body, parsing stops — any unparsed envelopes are **not** ack'd and the gateway treats them as lost (retry on next poll).

**Command ID** is a gateway-assigned `uint16_t` that is opaque to the node. The gateway uses it to correlate acks with outstanding queue entries. IDs wrap at 65535; the gateway should not keep any single command enqueued long enough for a wrap to cause ambiguity (queue depth of 8 makes this trivially safe).

### Opcode Registry

| Value | Name | Payload | Coalescing | Description |
|-------|------|---------|------------|-------------|
| `0x00` | reserved | — | — | — |
| `0x01` | `OP_SET_NUMBER` | `[name_len:1][name:N][value:float32 LE]` | Yes | Set a numeric entity (number/input) by ESPHome name |
| `0x02` | `OP_SET_SWITCH` | `[name_len:1][name:N][value:uint8]` | Yes | Set a switch entity: `0x00` off, `0x01` on |
| `0x03` | `OP_INVOKE` | `[name_len:1][name:N]` | No | Trigger a button/action entity (one press per enqueue) |
| `0x04`–`0x7F` | reserved | — | — | Future standard opcodes |
| `0x80`–`0xFF` | user | — | declared at reg. | Application-specific commands |

All current standard opcodes address entities by ESPHome `name`, matching the same name↔key convention used for sensor data (see *Sensor Data Payload Format*). `name_len` is a single byte (max 255 chars), though practical names are much shorter.

### Delivery Guarantees

- **At-least-once for non-coalescing commands** (`OP_INVOKE`, user-declared non-coalescing). If an ack is missing, the gateway retains the queue entry and retries on the next poll to that node. A node refusing the command (non-`ACK_OK` result) is terminal, not retried.
- **Latest-wins for coalescing commands**. The head-of-queue entry is retried until ack'd, but any fresh submission of the same `(opcode, target)` overwrites its payload in place before the next delivery attempt.
- **Bounded latency**: worst-case one `poll_interval` per delivery attempt; retries add a further `poll_interval` each.
- **No ordering across nodes.** Per-node FIFO is preserved. If two commands must be ordered across different nodes, the gateway API must serialize them externally.
- **Reboot-volatile.** Gateway reboot discards all queued commands. Callers needing reboot-durable control should re-submit on detecting a restart (e.g., via the gateway's own liveness signals in Home Assistant).

### Ack Flow

On each poll:

1. Gateway dequeues up to `MAX_COMMANDS_PER_POLL` commands for the target node, packs them into the poll request after the `cmd_count` byte, and marks them **in-flight** (not removed).
2. Node receives the poll, parses envelopes, executes (or refuses) each in order, and builds the poll response with one `COMMAND_ACK_KEY` record per received envelope *before* any sensor data.
3. Gateway parses acks from the response. For each `cmd_id`:
   - `ACK_OK` → remove the command from the queue.
   - Any other result → remove the command from the queue (terminal, no retry).
   - No ack for an in-flight `cmd_id` → un-mark (clear in-flight) and retain for retry on the next poll.
4. If the node's response is missed entirely (gateway timeout), all in-flight commands for that node are un-marked and retried on its next poll.

Because acks ride inside the poll response, they inherit the response packet's SipHash auth and per-node sequence-window replay protection — no additional anti-replay infrastructure is needed.

### Interaction with Listen Windows

Command delivery is fully compatible with listen-window power saving: commands arrive in the same packet the node is already awake to receive. A poll carrying commands is larger (a few tens of extra bytes per command), so RX airtime within the window grows slightly, but the window's **start time and duration are unchanged** — no adjustment to `listen_window` is required. The node's sleep/wake cadence is unaffected.

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
2. Gateway broadcasts time sync with schedule (poll_interval, slot_duration, node list) — requires `time_sync_interval` configured
3. Remote node finds its `slot_index` in the address list
4. First poll expected at `(slot_index + 1) * slot_duration` after time sync
5. Radio transitions to windowed mode: sleep → wake at window_start → listen → respond → sleep
6. Timing re-anchors on each successful poll reception (not just time syncs)

Once a remote is in windowed mode, its listen window is centered on its *own* poll arrival, not on cycle start. This means subsequent time sync broadcasts at the top of each cycle are usually **not heard** by scheduled remotes (only by nodes still in continuous RX). Time sync is therefore effectively a bootstrap/re-acquisition mechanism; steady-state re-anchoring happens via the polls themselves.

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
| `time_sync_interval` | time | No | — | Interval between time sync broadcasts. **Required** for listen-window remote nodes to acquire a schedule; without it they stay in continuous RX. |
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
| `full_update_interval` | int | No | `10` | Polls between forced full updates (1-255) |
| `sensors` | list | No | `[]` | Sensor IDs to report |
| `binary_sensors` | list | No | `[]` | Binary sensor IDs to report |

## Protocol Constants

```
AUTH_KEY_SIZE            = 16      bytes
AUTH_TAG_SIZE            = 2       bytes
SEQ_NUM_SIZE             = 2       bytes
AUTH_OVERHEAD            = 4       bytes
SEQ_WINDOW_SIZE          = 256
TX_SEQ_RESERVE_CHUNK     = 256     (gateway NVS reservation stride)
MAX_PACKET_SIZE          = 255     bytes
MAX_PAYLOAD_SIZE         = 246     bytes
POLL_REQUEST_MIN_SIZE    = 7       bytes (bare poll, cmd_count=0)
ACK_PACKET_SIZE          = 7       bytes
TIME_SYNC_HEADER_SIZE    = 14      bytes (before node list)
SLOT_MARGIN_MS           = 200     ms
DEFAULT_GUARD_WINDOW     = 50      ms
MAX_MISSED_POLLS         = 5

COMMAND_ACK_KEY          = 0x03
COMMAND_ENVELOPE_HEADER  = 4       bytes (opcode + cmd_id + payload_len)
COMMAND_ACK_RECORD_SIZE  = 4       bytes (key + cmd_id + result)
COMMAND_QUEUE_DEPTH      = 8       per-node FIFO, drop-oldest
MAX_COMMANDS_PER_POLL    = 8       also bounded by remaining packet space
```
