# Solar Monitor Circuit Connections (v3)

## Overview
Solar-powered monitoring system for Wyze Cam Pan v3. Uses a CN3722-based MPPT charge controller module for battery charging, with custom circuitry limited to monitoring and load switching.

**Key Design Features:**
- CN3722 module handles all charge regulation (CC/CV, MPPT, reverse protection)
- Dual INA219 monitoring: battery net current (SOC) + load current
- Solar panel voltage via resistor divider (generation indicator)
- Low-side N-channel MOSFET load switch
- Temperature monitoring
- 12V LiFePO4 battery

---

## Bill of Materials

### Resistors
| Ref | Value | Purpose |
|-----|-------|---------|
| R1 | 220kΩ | Voltage divider high side |
| R2 | 22kΩ | Voltage divider low side |
| R3 | 100Ω | M1 gate current limiting |
| R4 | 10kΩ | M1 gate pulldown |
| R5 | 4.7kΩ | DS18B20 pullup (if needed) |

### Semiconductors
| Ref | Part | Purpose |
|-----|------|---------|
| M1 | IRLZ44N | Load switch MOSFET |
| IC1 | INA219 @ 0x40 | Battery net current sensor (SOC) |
| IC2 | INA219 @ 0x41 | Load current sensor |
| U1 | DS18B20 | Temperature sensor |

### Modules
| Item | Notes |
|------|-------|
| MCU | Heltec WiFi LoRa 32 V3 (socketed) |
| CN3722 | MPPT solar charge controller (off-board, ~30x50mm) |
| LM2596 | Buck converter 12V→5V (off-board, 66x36mm) |

### Connectors
- 5mm pitch screw terminals: SOL+/-, BAT+/-, LOAD+/-
- Female headers for MCU (2x 18-pin)
- Female headers for INA219 boards (2x 6-pin)

---

## Removed from v2

| Component | Reason |
|-----------|--------|
| M1 (charge MOSFET) | CN3722 handles charge switching |
| R3, R4 (M1 gate resistors) | No longer needed without charge MOSFET |
| D1 (1N5822 Schottky) | CN3722 has internal reverse current protection |
| GPIO4 (PWM out) | No firmware charge control needed |

*Note: M1/R3/R4 ref designators have been reassigned to the load switch components (previously M2/R5/R6 in v2).*

---

## GPIO Assignments

| GPIO | Function | Header | Pin | Description |
|------|----------|--------|-----|-------------|
| GPIO5 | ADC In | J3 | 16 | Solar voltage via divider |
| GPIO41 | I2C SDA | J3 | 8 | Shared data: IC1, IC2 |
| GPIO42 | I2C SCL | J3 | 9 | Shared clock: IC1, IC2 |
| GPIO47 | 1-Wire | J2 | 13 | DS18B20 temperature |
| GPIO48 | Digital Out | J2 | 14 | M1 gate (load switch) |

**Routing Strategy:**
- J3 (left side) = sensing/input signals (ADC, I2C)
- J2 (right side) = control/output signals (load switch, temp sensor, 5V power)

---

## Connection List

### Solar Input (SOL)
```
SOL+ → CN3722 IN+
SOL+ → R1 (voltage divider)
SOL- → CN3722 IN-
```

### Voltage Divider (Solar Monitoring)
```
R1: SOL+ ↔ GPIO5
R2: GPIO5 ↔ GND

Ratio: 22k / (220k + 22k) = 0.0909
Max input: 22V → 2.0V at GPIO5
```

### CN3722 (Off-Board Charge Controller)
```
IN+  → SOL+
IN-  → SOL-
OUT+ → IC1 VIN+
OUT- → GND
```

**Important:** Verify charge voltage is set correctly for LiFePO4 (14.4-14.6V cutoff, ~13.6V float). Adjust feedback resistors on the CN3722 module if needed.

### IC1 (INA219 @ 0x40 - Battery Net Current)
**Purpose:** Measures net current into/out of battery for coulomb counting (SOC estimation). Reads positive when charging, negative when discharging.

```
VIN+ → CN3722 OUT+
VIN- → BAT+
VCC  → 3.3V
GND  → GND
SDA  → GPIO41
SCL  → GPIO42
```

**Shunt resistor:** 0.1Ω (R100)

### Battery (BAT)
```
BAT+ → IC1 VIN-
BAT+ → IC2 VIN+
BAT+ → LM2596 VIN+
BAT- → GND
```

### IC2 (INA219 @ 0x41 - Load Current)
**Purpose:** Measures current consumed by load (Wyze Cam via USB buck converter)

```
VIN+ → BAT+
VIN- → LOAD+
VCC  → 3.3V
GND  → GND
SDA  → GPIO41
SCL  → GPIO42
```

**Shunt resistor:** 0.1Ω (R100)

### M1 (Load Switch MOSFET)
```
Gate   → R3 → GPIO48
Gate   → R4 → GND (pulldown)
Drain  → LOAD-
Source → GND
```

### Load (Wyze Cam Pan v3 via 12V-to-5V USB converter)
```
LOAD+ → IC2 VIN-
LOAD- → M1 Drain
```

### DS18B20 (Temperature Sensor)
```
VCC  → 3.3V
DATA → GPIO47
DATA → R5 (4.7kΩ) → 3.3V (if not built into module)
GND  → GND
```

### LM2596 (Off-Board Buck Converter - MCU Power)
```
VIN+  → BAT+
VIN-  → GND
VOUT+ → MCU 5V (J2 pin 2)
VOUT- → GND
```

### MCU Power
```
5V (J2 pin 2)     ← LM2596 VOUT+
GND (J2 pin 1)    → GND bus
3.3V (J3 pins 2-3) → IC1 VCC, IC2 VCC, DS18B20 VCC
```

---

## Ground Connections (Star Topology)

All grounds connect independently to the GND bus:

**Power Grounds (heavy wire):**
- BAT-
- CN3722 OUT-
- M1 Source
- LM2596 VIN-
- LM2596 VOUT-

**Signal Grounds:**
- MCU GND (J2 pin 1)
- IC1 GND
- IC2 GND
- DS18B20 GND
- R2 (voltage divider)
- R4 (M1 pulldown)

---

## Current Flow Paths

### Charging Path (CN3722 Regulated)
```
SOL+ → CN3722 IN+ → [MPPT conversion] → CN3722 OUT+
     → IC1 VIN+ → [shunt measures net battery current] → IC1 VIN-
     → BAT+

SOL- → CN3722 IN-
     → GND

BAT- → GND (completes circuit)
```

### Load Path (M1 ON)
```
BAT+ → IC2 VIN+ → [shunt measures load current] → IC2 VIN-
     → LOAD+ → LOAD-
     → M1 Drain → M1 Source → GND

BAT- → GND (completes circuit)
```

### MCU Power Path
```
BAT+ → LM2596 VIN+ → [12V to 5V] → VOUT+ → MCU 5V
MCU internal regulator → 3.3V → sensors
```

**Note:** MCU power draw (~0.5W) flows through IC1 since the LM2596 taps BAT+ downstream of the IC1 shunt. This is captured in SOC coulomb counting automatically.

*Wait — this is incorrect. The LM2596 connects directly to BAT+, not through IC1. See note below.*

### IC1 Measurement Scope
IC1 sits between CN3722 OUT+ and BAT+. It measures:
- **Charging:** Current flowing from CN3722 into the battery (positive)
- **Not measured:** Current drawn from BAT+ by the LM2596 and load circuit, since those tap BAT+ on the battery side of IC1

This means IC1 only sees charge current, not discharge. For true coulomb counting you would need IC1 on the battery's positive terminal with all loads on one side and all charging on the other — but that would require both the CN3722 output and the load/LM2596 to route through the same shunt.

**Options:**
1. Accept IC1 as charge-only measurement. Use IC2 + known MCU draw to estimate discharge. Simple, less accurate.
2. Rearrange so IC1 sits directly on BAT+ with CN3722 on one side and all loads on the other. Accurate coulomb counting but more complex routing.

---

## Specifications

### Solar Panel
- Open circuit: 21.96V
- Operating: 13-15V
- Max current: 2A (25W panel)

### Battery (LiFePO4 12V, target 12-20Ah)
- Full: 14.4-14.6V
- Nominal: 13.0-13.8V
- Empty: 10.0-11.0V

### Load (Wyze Cam Pan v3)
- Input: 5V via 12V-to-USB converter
- Steady state: ~0.3A @ 5V (~1.5W)
- Peak (pan/tilt): ~2A @ 5V (~10W, brief)
- Estimated average: ~4W (with night vision, occasional motion)

### LM2596 (MCU Power)
- Input: 12V
- Output: 5.0V
- ESP32 draw: 80-150mA typical

---

## Changes from v2

1. **Removed M1/R3/R4 (charge MOSFET + gate resistors)** — CN3722 handles charge switching
2. **Removed D1 (1N5822 Schottky)** — CN3722 has internal reverse current protection
3. **Removed GPIO4 (PWM out)** — no firmware charge control
4. **IC1 repositioned** — now between CN3722 output and BAT+ for charge current measurement
5. **Load changed** from 10W LED to Wyze Cam Pan v3 (5V via USB converter)
6. **CN3722 module added** as off-board charge controller
7. **Ref designators renumbered** — M1 is now load switch, R3/R4 are load gate resistors, R5 is DS18B20 pullup

---

## Open Design Decision

**IC1 placement for SOC accuracy** — see "IC1 Measurement Scope" section above. Current placement only measures charge current. True net coulomb counting requires routing all current (charge and discharge) through a single shunt on the battery terminal.

---

## Author

Copyright 2025, Michael Green