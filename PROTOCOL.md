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
| `0xFF` | Reserved / invalid (formerly broadcast — see note) |
| `0x00` | Reserved / invalid |

Addresses are manually assigned. The user is responsible for uniqueness.

**There are no broadcast frames.** Every packet is unicast between the gateway
and exactly one node. `0xFF` was previously the broadcast destination for time
sync; the protocol no longer transmits to it, and a node must reject any packet
whose destination is not its own address. Anything historically modeled as
"broadcast" (time/schedule distribution, fan-out commands) is now delivered
per-node inside each node's own poll exchange.

## Byte Order

All multi-byte integers use **little-endian** byte order.

## Authentication

All packets are authenticated using **SipHash-2-4** with a mandatory 128-bit (16-byte) pre-shared key configured identically on the gateway and all remote nodes.

### Auth Footer (8 bytes, appended to every packet)

```
Byte N-7:  Boot epoch [7:0]
Byte N-6:  Boot epoch [15:8]        (uint16_t LE)
Byte N-5:  Sequence number [7:0]
Byte N-4:  Sequence number [15:8]   (uint16_t LE)
Byte N-3:  Auth tag [7:0]
Byte N-2:  Auth tag [15:8]
Byte N-1:  Auth tag [23:16]
Byte N:    Auth tag [31:24]         (uint32_t LE)
```

The authenticated counter is the pair `(epoch, seq)`. **Boot epoch** is a
per-device counter persisted once per boot (see *Counter Persistence*); **seq**
resets to 0 each boot and increments per transmission (in RAM only).

**Signing procedure:**
1. Construct packet body (command-specific bytes)
2. Append the 2-byte boot epoch (little-endian)
3. Append the 2-byte sequence number (little-endian), increment sender's counter
4. Compute SipHash-2-4 over (body + epoch + seq) using the pre-shared key
5. Truncate the 64-bit hash to 32 bits (`hash & 0xFFFFFFFF`)
6. Append the 4-byte auth tag (little-endian)

**Verification procedure:**
1. Check packet size >= AUTH_OVERHEAD (8 bytes)
2. Split packet into `body_plus_counter` (all but last 4 bytes) and `tag` (last 4 bytes)
3. Compute SipHash-2-4 over `body_plus_counter`, compare truncated result to received tag
4. Extract epoch and seq from the 4 bytes before the tag
5. Validate `(epoch, seq)` against the receiver's per-sender state (below)

The 32-bit tag bounds blind forgery / random false-accept at ~1 in 4.3 billion
per packet (vs ~1 in 65 536 for the previous 16-bit tag), for 2 extra bytes.

### Anti-Replay Protection

Each receiver stores, per sender, the last accepted `(epoch, seq)`. Acceptance:

- **Newer epoch** (`(rx_epoch − stored_epoch) mod 2¹⁶ ∈ [1, 2¹⁵]`, i.e. forward
  within the modular half-space — **never** a plain `rx_epoch > stored_epoch`
  comparison): a new session — accept, reset the window to this packet, and treat
  the sender as freshly (re)started (the gateway uses this to re-baseline a
  rebooted node).
- **Same epoch**, and `(rx_seq − last_seq) mod 2¹⁶ ∈ [1, 2¹⁵]`: forward within the
  session — accept and advance `last_seq`.
- Otherwise (older epoch, or a non-forward seq): reject as replay / stale.

Both the epoch and seq tests are the **same** modular forward predicate over a
uint16 half-space — see `is_forward_u16(candidate, reference)` below. Implementing
either as a plain `>` comparison reintroduces a wraparound cliff (epoch 0xFFFF →
0x0000, or seq within a long-lived boot) that is decades out and impossible to
surface in testing, so both receivers MUST use the shared helper.
- The first packet ever seen from a sender initializes the state and is accepted.

This is **monotonic forward-only** acceptance over a half sequence-space, not a
bitmap window. Because the MAC prevents forgery, an attacker cannot advance a
receiver's counter, so there is no benefit to bounding how far forward a genuine
packet may jump — and the half-space tolerance (~32 000 consecutive losses)
maximises resilience on a lossy link while still rejecting every replayed or
out-of-session packet. The protocol is strictly polled (one in-flight packet per
direction per slot), so out-of-order delivery does not occur and a bitmap is
unnecessary.

The gateway tracks `(epoch, seq)` **per remote node** and uses a **per-node
outbound seq** so each node sees a dense `+1` sequence (not a sparse view of a
gateway-wide counter shared across all nodes). Remote nodes track a single
gateway `(epoch, seq)`.

### Counter Persistence

Each device persists only its **boot epoch** — a single NVS write per boot,
incremented at startup before the first transmission. `seq` is never persisted;
it restarts at 0 every boot, and the fresh epoch makes that safe.

This symmetric scheme replaces the gateway's former chunked `tx_seq` reservation
(`TX_SEQ_RESERVE_CHUNK`) entirely and, crucially, **fixes the solar-reboot
lockout**: when a brownout-prone remote restarts, its epoch advances, the gateway
sees a newer epoch on the next response and re-baselines immediately — no more
rejecting a rebooted node's packets until the gateway itself happens to reboot.
Epoch is 16-bit (65 536 boots; decades even at multiple reboots per day) and is
covered by the MAC, so it cannot be forged or rolled back by an attacker. When the
epoch counter eventually wraps (0xFFFF → 0x0000) it is simply the next forward step
under the modular comparison above — there is no exhaustion case to handle.

### Modular Forward Comparison

Both the anti-replay epoch and seq tests use a single shared predicate, defined
once in `lora_protocol.h` and used identically by gateway and remote node:

```cpp
// True if `candidate` is strictly forward of `reference` within the uint16
// modular half-space — i.e. (candidate - reference) mod 2^16 is in [1, 2^15].
// Wraparound-safe: handles 0xFFFF -> 0x0000 as an ordinary +1 step. Never use a
// plain `>` on epoch/seq, which would brick replay state at the wrap boundary.
static inline bool is_forward_u16(uint16_t candidate, uint16_t reference) {
  uint16_t delta = static_cast<uint16_t>(candidate - reference);
  return delta >= 1u && delta <= 0x8000u;
}
```

`is_forward_u16(rx_epoch, stored_epoch)` selects a newer session; within the same
epoch, `is_forward_u16(rx_seq, last_seq)` accepts a forward seq.

### Key Configuration

The `auth_key` config option accepts either format:
- **Hex string**: 32 hex characters (e.g., `"0102030405060708090a0b0c0d0e0f10"`)
- **Base64**: base64-encoded 16-byte key (e.g., `"AQIDBA..."`)

### Threat Model

- **Goal**: integrity and replay resistance on a private ISM-band link, not
  confidentiality. Payloads are **not encrypted** — sensor readings and commands
  travel in clear; SipHash provides authentication only. Don't put secrets in
  sensor names/values.
- **MAC strength**: 32-bit tag → ~1 in 4.3e9 forgery/false-accept per packet.
  Adequate for a low-stakes farm deployment; not a high-value target's defense.
- **Replay**: prevented by the authenticated `(epoch, seq)` counter. An attacker
  cannot forge a higher counter without the key, so cannot advance a receiver's
  window or inject a "newer" packet. Replaying a captured packet fails (its
  counter is ≤ the receiver's last accepted). Residual risk: a captured packet
  replayed into the *same slot before* the genuine copy is indistinguishable from
  it (identical bytes) — harmless, since accepting either is equivalent. Accepted.
- **Shared key**: a single PSK is shared by the gateway and all nodes. Physically
  capturing any node exposes the key, allowing impersonation of the gateway to
  all nodes and any node to the gateway. Per-node keys (the header already
  carries src/dst, so the receiver can select a key) would limit blast radius;
  deferred as out of scope for the current deployment.

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
| `0x01` | `CMD_POLL_REQUEST` | Gateway → Node | Request sensor data; carries schedule, optional time, optional downlink commands |
| `0x02` | `CMD_POLL_RESPONSE` | Node → Gateway | Sensor data response; carries flags and optional command acks |

Only two frame types exist; both are unicast. The poll request is the gateway's
sole downlink and absorbs everything that previously needed extra frames —
schedule (`poll_interval`), wall-clock time (on request), and downlink commands.
The former `CMD_TIME_SYNC` broadcast and `CMD_ACK` frames are removed.

### Poll Request Flags (Byte 3)

```
bit 0  TIME_PRESENT   an epoch-time block follows the schedule field
bit 1  RESPONSE_ACKED the gateway received this node's previous poll response
bits 2-7  reserved (must be 0)
```

`RESPONSE_ACKED` replaces the old standalone ACK frame: receipt confirmation now
rides the next poll the node is already awake for, costing no extra airtime.

### Poll Request (17+ bytes, variable)

```
Byte 0:       Gateway address
Byte 1:       Target node address (unicast only — see below)
Byte 2:       0x01 (CMD_POLL_REQUEST)
Byte 3:       Flags (see above)
Bytes 4-7:    Poll interval in ms (uint32_t LE)         — schedule, always present
Bytes 8-11:   Unix timestamp, seconds since epoch (uint32_t LE)  — only if TIME_PRESENT
Byte M:       Command count (uint8_t; 0 = no commands)
Bytes M+1..:  Command envelopes (cmd_count × variable, see Downlink Commands)
Last 8:       Auth footer
```

`poll_interval` is carried on **every** poll so a node can (re)derive its entire
listen schedule from any single poll it receives — there is no separate schedule
frame and no node-list. The optional 4-byte time block is included only when the
gateway is answering a node's time request (see *Time Distribution*). A bare poll
(no time, no commands) is `3 + 1 + 4 + 1 + 8 = 17` bytes.

**Poll requests are unicast only.** The target address must equal the node's own
address; a node must reject any poll whose destination is not its address
(including `0xFF`). Simultaneous multi-node responses would collide, and commands
are distributed per-node, so there is never a reason to poll more than one node
at a time.

### Poll Response (10+ bytes)

```
Byte 0:    Source node address
Byte 1:    Gateway address
Byte 2:    0x02 (CMD_POLL_RESPONSE)
Byte 3:    Packet number (0x00 = single, 0x01+ = multi-packet fragment)
Byte 4:    Total packets (0x01 = single, 0x02+ = multi-packet)
Byte 5:    Flags (see below)
Bytes 6+:  Response payload (tagged records — sensor data, command acks, etc.)
Last 8:    Auth footer
```

### Poll Response Flags (Byte 5)

```
bit 0  TIME_REQUEST  node is requesting a wall-clock time update (see Time Distribution)
bits 1-7  reserved (must be 0)
```

Maximum payload per packet: **241 bytes** (255 max − 6 header − 8 auth). The
payload is a sequence of tagged records (see *Sensor Data Payload Format*); a
response to a poll that carried commands **must** begin with the corresponding
command acks before any other records.

### Time Distribution

There is no time-sync frame. A node that needs its clock set raises `TIME_REQUEST`
in its poll response; the gateway answers by setting `TIME_PRESENT` and appending
the 4-byte epoch to that node's **next** poll request. Cadence is node-driven and
documented under *Listen Windows → Time Sync Policy*.

## Sensor Data Payload Format

Sensor data is serialized as a sequence of tagged records. Each sensor value is identified on the wire by a **1-byte index**, not by name — the index is the 0-based position of the sensor in the node's declared entity list. Because the gateway already declares every remote node's sensors (it must, to register them with Home Assistant at boot) and the remote declares the same sensors in the same order, both sides share an identical index→sensor mapping with no per-value name overhead on the air.

**Index binding is positional.** Float sensors and binary sensors occupy **separate** index spaces (the record's type tag disambiguates them): the *i*-th declared `sensors:` entry is float index *i*, and the *i*-th declared `binary_sensors:` entry is binary index *i*, on both gateway and remote. The remote's declaration order **must** match the gateway's, or values will be published to the wrong entities. The ESPHome `name`↔`key` convention is retained as the human-facing declaration contract and as the basis for the optional *Schema Fingerprint* (below); names themselves no longer travel on the wire.

Records are serialized as:

### Schema Fingerprint (optional)

```
Byte 0:    0x04 (SCHEMA_FINGERPRINT_KEY)
Bytes 1-2: Fingerprint (uint16_t LE)
```

Because index binding is positional, a mismatch between the gateway's and remote's declaration order (or object_ids) would silently publish values to the wrong entities. To catch this, a node **may** emit a single schema-fingerprint record. The fingerprint is the low 16 bits of SipHash-2-4 (using the shared auth key) over the node's ordered manifest: for each declared sensor, then each declared binary sensor, append `[id_len:1][id:N]`, where `id` is the entity's `object_id` on the remote (`get_object_id()`) — equal by convention to the gateway's declared `object_id` for the same entity.

The gateway computes the expected fingerprint from its own declarations for that node. On mismatch it logs a configuration error and **drops the sensor data** (command acks are still processed); a matching or absent fingerprint passes. If present, this record appears immediately after any command-ack records and before the first sensor/binary record.

### Float Sensor (6 bytes)

```
Byte 0:    0x01 (SENSOR_KEY)
Byte 1:    Sensor index (uint8_t, 0-based position in the declared `sensors:` list)
Bytes 2-5: Float value (IEEE 754, 4 bytes LE)
```

### Binary Sensor (3 bytes)

```
Byte 0: 0x02 (BINARY_SENSOR_KEY)
Byte 1: Sensor index (uint8_t, 0-based position in the declared `binary_sensors:` list)
Byte 2: Boolean value (0x00 = false, 0x01 = true)
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

If serialized sensor data exceeds 241 bytes, it is split across multiple response packets:
- Fragment 1: `packet_num=0x01`, `total_packets=N`
- Fragment 2: `packet_num=0x02`, `total_packets=N`
- ...
- Single-packet responses use `packet_num=0x00`, `total_packets=0x01`
- 15 ms delay between fragment transmissions for gateway RX turnaround

The gateway reassembles fragments by source address, concatenating payloads in order. Incomplete assemblies are discarded on timeout.

### Full Snapshot Updates

Every poll response carries a **complete snapshot** of all sensors that currently have a valid state — there is no per-value change tracking or delta encoding. Index-based identification (above) makes a full snapshot cheap: a float record is 6 bytes and a binary record 3 bytes, so even a sensor-rich node fits a full update in a single packet at typical counts.

Sending a full snapshot every poll (rather than only changed values) is a deliberate tradeoff:

- **No stale-on-loss hazard.** A dropped response costs one poll of latency, not up to *N* polls. This matters most for binary sensors (door, water-level alarm, relay-tripped), where a stale value is a real fault and packet loss is the *expected* condition on a long-range 915 MHz link — not an edge case.
- **No cache state.** The remote keeps no last-sent cache and the gateway needs no resync logic; every response is self-contained and idempotent.
- **Marginal airtime cost.** With names off the wire, the airtime saved by suppressing unchanged values is small — small payloads often fall in the same LoRa symbol bucket either way, and the preamble/header floor dominates. Continuously-varying analog sensors (battery voltage, charge/load current) change nearly every poll and would be transmitted regardless, so delta encoding would mostly skip only the rarely-changing binaries — exactly the values whose staleness is most costly.

A node with no sensors in a valid state sends an empty payload; the gateway still records liveness/RSSI/SNR from the packet itself.

## Downlink Commands

The gateway can send commands to remote nodes (set a value, toggle a switch, invoke an action). Because remotes duty-cycle their radios and are only guaranteed to be listening around their own poll slot, commands are **piggybacked onto the poll request** for the target node. A node's next command-delivery opportunity is therefore bounded by one `poll_interval`.

There are **no broadcast frames** at all. "Broadcast a command to all nodes" is modeled at the gateway as a fanout across per-node queues, not as a single radio transmission — each node receives its copy in its own poll.

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

**Coalescing**: when a submitted command's `(opcode, target_object_id)` matches an entry already in the queue *and* the opcode is declared coalescing, the existing entry's payload is updated in place (queue position preserved). This keeps idempotent state-setters from stacking up behind a disconnected node — only the latest desired state survives. Non-coalescing opcodes always append.

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
| `0x01` | `OP_SET_NUMBER` | `[object_id_len:1][object_id:N][value:float32 LE]` | Yes | Set a numeric entity (number/input) by object_id |
| `0x02` | `OP_SET_SWITCH` | `[object_id_len:1][object_id:N][value:uint8]` | Yes | Set a switch entity: `0x00` off, `0x01` on |
| `0x03` | `OP_INVOKE` | `[object_id_len:1][object_id:N]` | No | Trigger a button/action entity (one press per enqueue) |
| `0x04`–`0x7F` | reserved | — | — | Future standard opcodes |
| `0x80`–`0xFF` | user | — | declared at reg. | Application-specific commands |

All current standard opcodes address entities by their ESPHome `object_id` — the sanitized slug returned by `get_object_id()` (lowercase, underscore-separated, derived from the entity's `name:`), **not** the compile-time `id:` C++ identifier (which has no runtime string form) and **not** the friendly `name:` (which may carry spaces, capitals, or UTF-8). The remote resolves a command by matching its `object_id` against each commandable entity's `get_object_id()`. Note this differs from the index-based identification used for **sensor reporting** (see *Sensor Data Payload Format*): commandable entities (numbers, switches, buttons) are a separate, potentially sparser set than reported sensors and are not currently declared in an ordered list on the gateway, so commands carry the object_id inline. `object_id_len` is a single byte (max 255 chars), though practical object_ids are much shorter. *(A future revision may give commandable entities their own index space if airtime on the downlink ever warrants it.)*

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
- Schedule (`poll_interval`) rides every poll; wall-clock time is appended to a
  node's poll only when that node has requested it (see *Time Distribution*)

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
                        │   first poll)    │
                        └───────┬──────────┘
                                │ first poll received
                                │ (carries poll_interval)
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

`poll_interval` is taken from the most recent poll (it rides every poll), and the
window re-anchors on every successful reception — so the only drift that matters
is what accumulates between two *successfully received* polls.

**Guard window derivation.** The window must cover clock drift over one
`poll_interval` plus fixed jitter:

```
guard_window ≈ 2·(clock_ppm × 1e-6 × poll_interval)   ← drift, scales with interval
             + 16 ms   (ESPHome loop() jitter)
             +  0.5 ms (SX1262 STDBY_RC → RX)
             + 10 ms   (safety padding)
```

Drift scales with `poll_interval`; the jitter terms do not. At short intervals
loop jitter dominates and ~50 ms suffices. At long intervals a poor clock
dominates:

| Clock | ppm | Drift @ 30 s | Drift @ 10 min |
|-------|-----|-------------|----------------|
| DS3231 (TCXO) | ±2 | ±0.06 ms | ±1.2 ms |
| Bare ESP32 osc (outdoor temp) | ±50 | ±1.5 ms | ±30 ms |

**Consequence:** a bare-oscillator node at a long `poll_interval` drifts out of a
50 ms window and lands in fallback every cycle, burning the power that windowing
exists to save. Therefore, when `listen_window` is enabled the node **requires an
RTC** (`has_rtc: true`) unless the computed guard for its `poll_interval` and
clock class still fits a sane window; the gateway/remote validate this at startup
and warn or refuse. `clock_ppm` is derived from `has_rtc` (≈2 with RTC, ≈50
without) and may be overridden; `listen_window` may be given explicitly to
override the computed guard.

### Schedule Acquisition

1. Remote node powers on in **continuous RX**.
2. The gateway polls it in round-robin like any node; the **first poll it
   receives** carries `poll_interval`, which is all it needs.
3. It records the poll arrival time and transitions to windowed mode: sleep →
   wake at `window_start` → listen → respond → sleep.
4. Timing re-anchors on every successful poll reception.

There is no schedule broadcast, node-list, or slot-index — each node only needs
*when its own next poll arrives* (`last_poll + poll_interval`), never the network
topology. This also closes the former time-sync topology leak.

### Fallback

After **5 consecutive missed polls** (`MAX_MISSED_POLLS`), the node falls back to
continuous RX to re-acquire. This handles gateway restarts, schedule changes,
accumulated drift, and RF interference.

**Re-acquisition power cost.** Without a broadcast to catch, a fallen-back node
must stay in continuous RX until **its own** next unicast poll comes around —
up to one `poll_interval` of full-power RX. Acceptable for stable nodes; a node
that repeatedly flaps in and out of fallback at a long interval will spend
meaningful time in continuous RX, so size `poll_interval`/`listen_window` (and
fit an RTC) to keep nodes reliably in-window.

### Time Sync Policy

Wall-clock time is delivered on demand (see *Time Distribution*). A node decides
when to raise `TIME_REQUEST` based on its own hardware — the gateway needs no
knowledge of which nodes have an RTC:

| Node | Behaviour | Rationale |
|------|-----------|-----------|
| **No RTC** | Request after every boot (clock is unset), then every `time_refresh_interval` (default ~1 h) | Crystal drift ~1.7 s/day typ., ~4 s/day worst-case outdoors; loses time on every brownout |
| **With DS3231** | Request once after boot only if the RTC lost power (oscillator-stop flag); otherwise never | TCXO holds ~1 s per 6 days and is battery-backed across reboots |

The gateway may also enforce a per-node `time_sync_interval` ceiling as a
belt-and-suspenders backstop. Time sync is purely a wall-clock concern — it is
**not** needed for listen-window anchoring, which the polls handle.

## Radio Parameters

Spreading factor (SF), bandwidth (BW), and coding rate (CR) are configured on the
`sx126x` component, not here — but the protocol's timing depends on them, so they
are documented for cross-reference. The gateway and every node **must** use
identical SF/BW/CR (and sync word). Time-on-air follows the Semtech LoRa airtime
equation; payload airtime is quantized into symbols, so trimming a few bytes
rarely saves airtime while changing SF does so exponentially.

**Derived quantities that depend on SF/BW/CR:**
- `response_timeout` must exceed the worst-case poll-response airtime (largest
  snapshot, including any multi-packet fragmentation) plus RX turnaround.
- `slot_duration = response_timeout + SLOT_MARGIN_MS`, hence `cycle_duration`
  and the minimum `poll_interval`.

**Sizing guidance (optimize for link margin, not data rate):** payloads are tiny
and the poll interval is minutes, so a slower SF costs nothing that matters.
At 125 kHz, rough sensitivity is SF7 ≈ −123 dBm, SF9 ≈ −129, SF12 ≈ −137
(each +1 SF ≈ +2.5 dB reach for ~2× airtime). Pick the SF that keeps the
**worst-placed node in its worst position** at ≥ 15–20 dB margin. For a
sub-¼-mile farm link with some obstruction, **SF9 / 125 kHz / CR 4/5** is a
reasonable starting point. Don't guess — drive the mobile coop to its worst
position and read RSSI/SNR from the gateway's liveness metrics at SF7, then let
the required SF fall out of that measurement. Per-node SF is possible but makes
`slot_duration` per-node (the gateway must retune between slots); defer until a
node genuinely needs SF11+ while others are fine.

## Configuration Reference

### Gateway (`lora_gateway`)

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `sx126x_id` | ID | Yes | — | SX126x radio component (SF/BW/CR set here — see *Radio Parameters*) |
| `address` | hex uint8 | Yes | — | Gateway address (0x01–0xFE) |
| `auth_key` | string | Yes | — | 16-byte key (hex or base64) |
| `response_timeout` | time | Yes | — | Max wait for poll response (must exceed worst-case response airtime) |
| `poll_interval` | time | Yes | — | Time between polling cycles |
| `time_id` | ID | No | — | RealTimeClock the gateway reads to answer node time requests |
| `response_acked` | bool | No | `true` | Set the `RESPONSE_ACKED` flag on the next poll after a received response |
| `stale_sensor_behavior` | enum | No | `keep` | `keep` or `invalidate` on timeout |
| `remote_nodes` | list | Yes | — | List of remote node definitions |

Each entry in `remote_nodes` may carry a `time_sync_interval` (optional) — a
ceiling on how often the gateway will push time to that node even absent a
`TIME_REQUEST`; omit for purely demand-driven behaviour.

### Remote Node (`lora_remote_node`)

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `sx126x_id` | ID | Yes | — | SX126x radio component (must match the gateway's SF/BW/CR) |
| `address` | hex uint8 | Yes | — | Node address (0x01–0xFE) |
| `auth_key` | string | Yes | — | 16-byte key (hex or base64) |
| `time_id` | ID | No | — | `lora_remote_node` time platform set from gateway time syncs |
| `has_rtc` | bool | No | `false` | Node has a battery-backed RTC (DS3231); selects clock class and time-sync cadence |
| `clock_ppm` | int | No | from `has_rtc` | Override clock drift estimate for guard-window sizing (≈2 RTC, ≈50 bare) |
| `listen_window` | time | No | computed | Guard window override; default computed from `poll_interval` and `clock_ppm` |
| `time_refresh_interval` | time | No | `1h` | For RTC-less nodes: how often to request a fresh wall-clock time |
| `sensors` | list | No | `[]` | Sensor IDs to report (declaration order defines float index) |
| `binary_sensors` | list | No | `[]` | Binary sensor IDs to report (declaration order defines binary index) |

> When `listen_window` is enabled, an RTC is required unless the guard window
> computed from `poll_interval` and `clock_ppm` still fits a sane bound — see
> *Listen Windows → Guard window derivation*.

## Protocol Constants

```
AUTH_KEY_SIZE            = 16      bytes
EPOCH_SIZE               = 2       bytes (boot epoch)
SEQ_NUM_SIZE             = 2       bytes
AUTH_TAG_SIZE            = 4       bytes (32-bit truncated SipHash)
AUTH_OVERHEAD            = 8       bytes (epoch + seq + tag)
SEQ_FORWARD_WINDOW       = 0x8000  (32768; accept (seq-last) mod 2^16 in [1, this])
MAX_PACKET_SIZE          = 255     bytes
POLL_RESPONSE_HEADER_SIZE= 6       bytes (src + dst + cmd + pkt_num + total + flags)
MAX_PAYLOAD_SIZE         = 241     bytes (255 - 6 header - 8 auth)
POLL_REQUEST_MIN_SIZE    = 17      bytes (header + flags + poll_interval + cmd_count + auth)
SLOT_MARGIN_MS           = 200     ms
DEFAULT_GUARD_WINDOW     = 50      ms (used only when drift fits; else computed)
MAX_MISSED_POLLS         = 5

# Poll request flags (byte 3)
PREQ_FLAG_TIME_PRESENT   = 0x01
PREQ_FLAG_RESPONSE_ACKED = 0x02
# Poll response flags (byte 5)
PRESP_FLAG_TIME_REQUEST  = 0x01

SENSOR_KEY               = 0x01
BINARY_SENSOR_KEY        = 0x02
COMMAND_ACK_KEY          = 0x03
SCHEMA_FINGERPRINT_KEY   = 0x04
FLOAT_RECORD_SIZE        = 6       bytes (key + index + float32)
BINARY_RECORD_SIZE       = 3       bytes (key + index + value)
COMMAND_ENVELOPE_HEADER  = 4       bytes (opcode + cmd_id + payload_len)
COMMAND_ACK_RECORD_SIZE  = 4       bytes (key + cmd_id + result)
COMMAND_QUEUE_DEPTH      = 8       per-node FIFO, drop-oldest
MAX_COMMANDS_PER_POLL    = 8       also bounded by remaining packet space
```
