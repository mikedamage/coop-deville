# Protocol Revision — Implementation Plan

Tracking the robustness/unicast/indexing overhaul of the LoRa polling protocol.
Supersedes the relevant parts of the original `IMPLEMENTATION_PLAN.md` (kept as
history). Branch: `feat/protocol-robustness-revision`.

## Goal

Harden the protocol for a lossy 915 MHz link and brownout-prone solar nodes,
eliminate all broadcast traffic, and cut per-packet airtime — without giving up
replay protection. Concretely:

1. **Auth/replay**: boot-epoch counter + per-destination sequence + monotonic
   half-space window + wider MAC; fixes the solar-reboot replay lockout and
   deletes the NVS chunk-reservation machinery.
2. **All-unicast**: remove `0xFF`; fold timing into the (unicast) poll so nodes
   self-anchor; time sync becomes demand-driven and per-node.
3. **Payload**: full snapshots (no delta) with 1-byte index identification
   *(spec already revised; commit 57e275d)*.
4. **Listen window**: guard sized as a function of `poll_interval` and clock
   quality; require an RTC (or auto-widen) for power-saving nodes.
5. **Docs**: radio-parameter cross-reference + threat model.

## Wire format breaks — flag-day upgrade

Every change below alters the on-air format. Gateway and all remotes must be
reflashed together; there is no mixed-version interop. Acceptable for this
deployment. Each implementation phase must leave **both** components compiling
and wire-compatible *with each other*.

---

## Locked design decisions

- **Footer** becomes `[epoch:2 LE][seq:2 LE][tag:N LE]`. `epoch` is a per-device
  boot counter persisted once per boot; `seq` resets to 0 each boot (RAM only).
- **Acceptance rule** (per source): store `(last_epoch, last_seq)`.
  - `rx_epoch` newer → accept, reset window, treat as fresh session.
  - `rx_epoch == last_epoch` and `(seq − last_seq) mod 2¹⁶ ∈ [1, 2¹⁵]` → accept, advance.
  - otherwise → reject (older session / replay / stale).
- **Per-destination gateway counters**: gateway holds one boot `epoch` + a
  `tx_seq` *per RemoteNode*; each node sees a dense +1 sequence.
- **Drop** `TX_SEQ_RESERVE_CHUNK` and all chunked-NVS reservation logic — the
  epoch supersedes it on both sides.
- **No broadcast frames.** `0xFF` is retired to reserved/invalid. Timing rides
  inside the unicast poll; wall-clock time is sent only on request.
- **Self-anchoring**: a node derives its whole schedule from any single poll
  (carries `poll_interval`); no node-list, no slot index.
- **Full snapshots, indexed** *(done in spec)*: every poll returns all valid
  sensors as `[tag][index][value]`; optional schema-fingerprint record.

## Open decisions (need a call before/while coding)

| # | Decision | Options | Recommendation |
|---|----------|---------|----------------|
| D1 | MAC tag width | 16-bit (footer 6 B) vs 32-bit (footer 8 B) | **32-bit** — 1-in-4B forgery for +2 B |
| D2 | `poll_interval` in poll | every poll (+4 B) vs flag-gated | **every poll** — stateless, any poll re-anchors |
| D3 | `CMD_ACK` (0x04) | keep vs remove | **remove** — vestigial once time-request rides response flags and delta ACK-coupling is gone |
| D4 | Schema fingerprint | implement now vs defer | **implement now** — cheap guard against silent index mis-map |
| D5 | Clock-quality config | `has_rtc` bool vs raw `clock_ppm` | **`has_rtc`** preset (≈2 ppm RTC / ≈50 ppm bare), with optional ppm override |
| D6 | Duplicated `lora_protocol.h` | share vs keep two copies | **keep two**, sync by hand — cross-component sharing is awkward in ESPHome externals |

---

## Phase 0 — Finish `PROTOCOL.md` ✅ DONE

Decisions locked: D1 32-bit tag · D2 `poll_interval` every poll · D3 remove
`CMD_ACK` · D4 schema fingerprint now · D5 `has_rtc` preset · D6 keep both
`lora_protocol.h` copies in sync.

All sections below are now written into `PROTOCOL.md` (the spec is the source of
truth; code phases 1–6 implement it):

- **Authentication**: new footer; boot-epoch; monotonic half-space window;
  per-destination counters; tag width (D1); remove chunk persistence. Add a
  short **Threat Model** subsection (shared-key scope, MAC strength, accepted
  residual replay risk).
- **Addressing**: `0xFF` → reserved/invalid.
- **Commands table + Packet Formats**: remove the Time Sync Broadcast frame;
  redefine the unicast Poll Request (`flags` byte, `poll_interval`, optional
  epoch-time block); add a `flags` byte to the Poll Response (`TIME_REQUEST`,
  …); resolve `CMD_ACK` (D3). Update sizes/offsets.
- **Polling Timing**: no time-sync-at-cycle-start.
- **Listen Windows**: self-anchor from poll; drop node-list/slot acquisition;
  guard window as `f(poll_interval, ppm)`; RTC requirement; document the
  re-acquisition continuous-RX cost.
- **Time sync**: demand-driven via `TIME_REQUEST`; per-node cadence (RTC vs
  no-RTC) policy.
- **Configuration Reference**: per-node time-sync policy; `has_rtc`/`clock_ppm`;
  radio-param cross-reference.
- **Radio Parameters**: new section (SF/BW/CR drive airtime/slot/guard; SF9 @
  125 kHz/CR 4/5 starting point; size SF from a worst-position RSSI/SNR read).
- **Protocol Constants**: footer sizes, flag bits, remove `TX_SEQ_RESERVE_CHUNK`.

---

> **Status:** Phase 1 ✅ and Phase 2 ✅ landed on `feat/protocol-robustness-revision`.
> The **per-destination gateway seq** moved from Phase 1 to Phase 2: it is
> gateway-internal and invisible on the wire/to the remote, but incompatible with
> the (then still-present) broadcast time-sync frame, so it landed alongside
> broadcast removal. The half-space window covered the shared-counter interim.

## Phase 1 — Auth core (boot epoch, per-dest seq, window, tag)

**Files:** `components/{lora_gateway,lora_remote_node}/lora_protocol.h`,
`lora_gateway.{h,cpp}`, `lora_remote_node.{h,cpp}`.

- `lora_protocol.h` (both, keep identical): `EPOCH_SIZE = 2`; `AUTH_TAG_SIZE`
  2→4 (D1); `AUTH_OVERHEAD = EPOCH_SIZE + SEQ_NUM_SIZE + AUTH_TAG_SIZE`; remove
  `SEQ_WINDOW_SIZE` 256 usage in favor of the half-space rule; drop
  `TX_SEQ_RESERVE_CHUNK`.
- Boot epoch helper (both): in `setup()`, load `uint16 boot_epoch`, `++`, save
  once (`global_preferences`); keep in RAM.
- **Gateway** `lora_gateway.cpp`: replace `tx_seq_`/`tx_seq_reserved_`/
  `tx_seq_pref_` with `boot_epoch_` (persisted once) + per-`RemoteNode`
  `tx_seq_` (RAM). `sign_packet_(node, body)` now takes the destination so it
  can pick that node's counter and stamp `[epoch][seq]`.
- **Remote** `lora_remote_node.cpp`: `boot_epoch_` persisted; single RAM
  `tx_seq_`.
- Receiver: `RemoteNode` gains `rx_epoch`/`rx_seq` (gateway side already has
  `rx_seq_*` — extend); remote keeps single `(gw_epoch_, gw_seq_)`. Implement
  the acceptance rule in `check_seq_` / `check_gw_seq_`. New-session detection
  drives the "force fresh" path used later by indexing/time.
- `verify_packet_`/`sign_packet_` (both): pack/extract `epoch`+`seq`; tag over
  `body+epoch+seq`.

**Verify:** both test configs compile; on-bench round trip; simulate remote
reboot (epoch bump) and confirm the gateway re-accepts immediately.

## Phase 2 — All-unicast + self-anchoring poll

**Files:** gateway/remote `.cpp`/`.h`, both `__init__.py`.

- **Addressing**: remove `BROADCAST_ADDRESS` accept paths everywhere; reject
  `0xFF` as a destination.
- **Gateway**: delete `broadcast_time_sync_` and the cycle-start sync; extend
  `send_poll_request_` to emit `[gw][node][CMD_POLL][flags][poll_interval:4]`
  `(+[epoch_time:4] when flags.TIME)` `[cmd_count]…`. Remove node-list code.
- **Remote**: parse the new poll (flags/`poll_interval`/optional time); anchor
  the next listen window off the received poll + `poll_interval`; delete
  `is_time_sync_`/`handle_time_sync_` broadcast path and `slot_index_`/
  `schedule_node_count_`; simplify the state machine to
  continuous-RX → first-poll → windowed → miss×N → continuous-RX.
- **Poll Response**: add a `flags` byte after `total_packets`; bump
  `POLL_RESPONSE_HEADER_SIZE` 5→6 and recompute `MAX_PAYLOAD_SIZE` + offsets.
- Apply D3 for `CMD_ACK`.

**Verify:** node with no RTC and no prior schedule acquires purely from its
first unicast poll; fallback→reacquire works.

## Phase 3 — Delta removal + 1-byte indexing (implements committed spec)

**Files:** remote `.cpp`/`.h`, gateway `.cpp`/`.h`, both `__init__.py`.

- **Remote** `serialize_sensor_data_`: delete `last_sent_*` caches,
  `full_update_*`, `force_next_full_update_`; emit `[tag][index][value]` for
  every sensor/binary with valid state, index = declared position; optionally
  prepend the schema-fingerprint record (D4). Drop `full_update_interval` member.
- **Gateway** `RemoteNode`: change sensor storage from
  `std::map<std::string,Sensor*>` (keyed by `key`) to `std::vector<Sensor*>`
  (index → sensor) plus a parallel ordered key/name list for the fingerprint;
  `process_complete_response_` parses `[tag][index][value]`, bounds-checks the
  index, and (D4) validates the fingerprint before publishing.
- **`__init__.py`**: remove `full_update_interval` (remote); gateway registers
  sensors by index in declared order (still capturing `key` for fingerprint +
  HA name). Remote list already preserves order.
- Shared fingerprint helper: SipHash over the ordered `[len][name]…` manifest,
  computed identically on both sides.

**Verify:** values land on the correct HA entities; deliberately reorder one
side's declaration and confirm the fingerprint mismatch is logged and data
dropped.

## Phase 4 — Demand-driven time sync + per-node cadence

**Files:** remote/gateway `.cpp`/`.h`, both `__init__.py`.

- **Remote**: track clock validity (unset after boot for a bare node; DS3231
  oscillator-stop flag for an RTC node). Raise `flags.TIME_REQUEST` in the poll
  response per policy: always until first sync after boot; for no-RTC nodes also
  every `time_refresh_interval` (default ~1 h); RTC nodes only on boot/loss.
- **Gateway**: per-`remote_node` time policy (`time_sync_interval` override /
  on-request). When a node requests (or the interval elapses), set `flags.TIME`
  + epoch in that node's next poll. Requires gateway `time_id`.
- **Config**: per-node time policy + remote `has_rtc`/`time_refresh_interval`.

## Phase 5 — Guard window scaling + RTC requirement

**Files:** remote `.cpp`/`.h`, `__init__.py`.

- Compute `guard ≈ 2·(ppm·1e-6·poll_interval) + loop_jitter(16 ms) +
  radio_wake(0.5 ms) + margin(10 ms)`. `ppm` from `has_rtc` (D5).
- Validate `listen_window`/RTC against the learned `poll_interval`; warn or
  refuse power-saving on a bare oscillator at long intervals; allow an explicit
  guard override.

## Phase 6 — Radio params + tests + cleanup

- Spec section landed in Phase 0; optionally add config validation
  cross-checking `response_timeout` against computed airtime at the configured
  SF/BW/CR.
- Update `tests/test-lora-gateway.yaml` / `tests/test-lora-remote-node.yaml`
  for the new options; add a multi-sensor node to exercise indexing.
- Consider a host-native unit test for the auth/window and fingerprint logic
  (pure functions, no ESPHome runtime needed); otherwise rely on compile +
  on-hardware round-trip.

---

## Per-phase workflow (from CLAUDE.md)

1. `clang-format -i components/**/*.{h,cpp}`
2. `esphome compile tests/test-lora-gateway.yaml tests/test-lora-remote-node.yaml`
   (run `scripts/populate-secrets` if it complains about a missing secret;
   `esphome clean …` then retry on a linker error before touching config).
3. Commit per phase (own branch/PR if reviewing incrementally). Pre-commit hook
   needs `transcrypt`; absent in some envs → `--no-verify`.

## Risks / notes

- **Footer growth** 4→8 B (with 32-bit tag). Re-check `POLL_REQUEST_MIN_SIZE`,
  `MAX_PAYLOAD_SIZE`, and all size guards in `on_packet`.
- **Response header +1 B** (flags) shifts payload offsets — audit every
  hard-coded offset.
- **Keep the two `lora_protocol.h` copies in lockstep** (D6).
- Phases 1–3 are the P0 core (auth, unicast, payload); 4–6 are P1/P2 and can
  land later without blocking a first working deployment.
