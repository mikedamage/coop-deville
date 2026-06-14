# Project Todos

## Active
- [ ] Design and build external component implementing round robin polling of remote nodes over LoRa to replace packet_transport
- [ ] LoRa time sync only sets the ESP system clock (via `synchronize_epoch_`). If a remote node's `time_id` is ever pointed at a hardware RTC chip (e.g. DS3231), persisting the gateway-supplied time into the chip across deep sleep needs an additional `write_time()` step — not currently done. System wall-clock is sufficient for listen-window scheduling, so this is only relevant if chip-backed time persistence is wanted.

## Completed
