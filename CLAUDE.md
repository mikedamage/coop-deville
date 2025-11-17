# Project Summary

This project contains the ESPHome configuration files and dependencies for an ESP32 controlled, solar powered mobile chicken coop automatiom system.

The remote node handles solar battery charging and directly controls power to its load based on input/output voltage and current, ambient temperature, light levels, time of day, etc. These units are not guaranteed to have a stable wifi connection, and as such they are only intended to use wifi for OTA updates and remote debug log access; they are equipped with LoRa radios (SX1262, 915MHz) for remote telemetry and control.

Configuration also exists for a gateway node that will be placed in a location where it has reliable wifi signal. The gateway is responsible for polling remote nodes over LoRa and forwarding their sensor states to Home Assistant, as well as transmitting control messages to the remote nodes.

# Coding Conventions

- Whenever possible, I prefer to avoid lambdas and use declarative YAML configuration.
- If more than one configration uses substantially similar code, I attempt to refactor it into a package and use substitutions to interpolate values that may differ between devices.

# Sensor Config files

## `chicken-tractor.yaml`

This is a PWM solar battery charger with an algorithm meant to approximate an MPPT charger's "perturb and observe," adjusting the duty cycle of a MOSFET between the solar panel and a 12v battery to maximize power transfer and handle transitions between charge phases. It is powered by a Heltec WiFi LoRa 32 v3.2 dev board.

A second MOSFET controls power to a 10 watt LED flood light that provides supplemental daylight to a mobile chicken coop. Two INA219 DC current sensors monitor charge current and load current, respectively. The ESP32 currently reads the solar panel voltage using one of its ADC's through a voltage divider

See `CIRCUIT_CONNECTIONS.md` for a parts lists and connections.

## `lora-gateway.yaml`

This is another Heltec WiFi LoRa 32 v3.2 board with no physical connections besides its 915MHz rubber duck antenna. Its job is to connect to the wifi network, poll remote nodes over LoRa in a round robin fashion, and forward their sensor states to Home Assistant or an MQTT broker. It also is capable of transmitting control signals (unicast or broadcast) to remote nodes and reports its own stats about LoRa and wifi signal quality.

# Workflow

- After every set of changes I have you make, compile the configuration to confirm that it's valid.
- If ESPHome throws an error about a missing secret during compilation, run `scripts/populate-secrets` and retry.
- In case of a linker error, do the following before attempting to fix the error by changing the config files:
  - Clean up the build environment
  - Try again to compile the configuration
  - If this second attempt fails, _then_ make changes to the config to attempt to fix the problem, clean the build environment again, and recompile.

See below for command examples.

# Commands

- `scripts/populate-secrets`: loads secrets from a 1Password secure note and writes them to the local @secrets.yaml file. If secrets.yaml exists, it is overwritten.
- `esphome compile config-file-name.yaml [another-config-file.yaml]`: Verifies, generates code, and compiles a binary for the specified device configuration file
- `esphome upload config-file-name.yaml [another-config-file.yaml]`: Uploads the most recently compiled binary to the specified device, identified by config file
- `esphome clean config-file-name.yaml [another-config-file.yaml]`: Wipes build environment and any artifacts from prior builds.
- `esphome logs config-file-name.yaml`: stream a device's logger output

