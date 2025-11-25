# Solar Charger Circuit Connections

## Overview
This document describes the complete wiring and connections for the LiFePO4 solar battery charger prototype built on breadboard.

**Key Design Features:**
- Low-side switching with N-channel MOSFETs
- Dual INA219 current/voltage monitoring (solar charging + load)
- PWM charge control
- Temperature monitoring
- 12V LiFePO4 battery support

---

## Bill of Materials (BOM)

### Resistors
- **R1:** 220kΩ, 5% tolerance (voltage divider - high side)
- **R2:** 22kΩ, 5% tolerance (voltage divider - low side)
- **R3:** 100Ω, 5% tolerance (M1 gate current limiting)
- **R4:** 10kΩ, 5% tolerance (M1 gate pulldown)
- **R5:** 100Ω, 5% tolerance (M2 gate current limiting)
- **R6:** 10kΩ, 5% tolerance (M2 gate pulldown)

### MOSFETs
- **M1:** IRLZ44N N-channel MOSFET (charge control PWM)
- **M2:** IRLZ44N N-channel MOSFET (load switch)

### Diodes
- **D1:** 1N5822 Schottky diode (reverse current protection)

### Sensors & ICs
- **IC1:** INA219 breakout board @ I2C address 0x40 (solar/charge current sensor)
- **IC2:** INA219 breakout board @ I2C address 0x41 (load current sensor)
- **DS18B20:** Digital temperature sensor (1-Wire protocol)

### Power Components
- **LM2596:** DC-DC buck converter module (12V → 5V)
- **SOL:** 12V 25W solar panel (or 14V 2A AC adapter for bench testing)
- **BAT:** 7Ah LiFePO4 12V battery
- **Load:** 10W LED flood light

### Microcontroller
- **MCU:** Heltec WiFi LoRa 32 v3 (ESP32-S3)

### Optional Components
- 4.7kΩ pullup resistor for DS18B20 (if not built into sensor module)

---

## Complete Connection List

### Solar Panel (SOL)
```
POS → R1 (voltage divider)
POS → IC1 VIN+
NEG → M1 Drain
```

### D1 (1N5822 Schottky Diode - Reverse Protection)
**Purpose:** Prevent reverse current from battery to solar panel at night

```
Anode → IC1 VIN-
Cathode → Battery POS
```

### Solar Voltage Divider (for ADC reading)
```
R1: Solar POS ↔ GPIO3
R2: GPIO3 ↔ Common GND

Voltage divider ratio: 22kΩ / (220kΩ + 22kΩ) = 0.0909
Max input: 22V → 2.0V at GPIO3
```

### Battery (BAT)
```
POS → D1 Cathode
POS → IC2 VIN+
POS → LM2596 VIN+
NEG → Common GND
NEG → LM2596 VIN-
```

### M1 (Charge Control MOSFET)
**Purpose:** PWM control of solar charging current (low-side switching)

```
Gate → R3 → GPIO4
Gate → R4 → Common GND (pulldown)
Drain → Solar NEG
Source → IC1 VIN+
```

**Gate resistor connections:**
```
R3: GPIO4 ↔ M1 Gate (current limiting)
R4: M1 Gate ↔ Common GND (pulldown)
```

### M2 (Load Switch MOSFET)
**Purpose:** Enable/disable power to load (LED flood light)

```
Gate → R5 → GPIO48
Gate → R6 → Common GND (pulldown)
Drain → Load NEG
Source → IC2 VIN-
```

**Gate resistor connections:**
```
R5: GPIO48 ↔ M2 Gate (current limiting)
R6: M2 Gate ↔ Common GND (pulldown)
```

### IC1 (INA219 @ 0x40 - Solar/Charge Current Sensor)
**Purpose:** Measures current flowing from solar panel to battery

```
VIN+ → Solar POS
VIN- → D1 Anode
SDA → GPIO1 (I2C data)
SCL → GPIO2 (I2C clock)
VCC → MCU 3.3V
GND → Common GND
```

**Shunt resistor:** 0.1Ω (R100)
**Measurement range:** 0-3.2A with PGA

### IC2 (INA219 @ 0x41 - Load Current Sensor)
**Purpose:** Measures current consumed by load

```
VIN+ → Battery POS
VIN- → Load POS
SDA → GPIO1 (I2C data)
SCL → GPIO2 (I2C clock)
VCC → MCU 3.3V
GND → Common GND
```

**Shunt resistor:** 0.1Ω (R100)
**Measurement range:** 0-3.2A with PGA

### Load (LED Flood Light)
```
POS → IC2 VIN-
NEG → M2 Drain
NEG → Common GND
```

**Specifications:** 10W @ 12V = ~0.83A draw

### DS18B20 (Temperature Sensor)
**Purpose:** Monitor battery temperature for charging algorithm

```
VCC → MCU 3.3V
DATA → GPIO47
DATA → 4.7kΩ pullup → 3.3V (if not built into module)
GND → Common GND
```

**Protocol:** 1-Wire digital communication

### LM2596 (Buck Converter)
**Purpose:** Convert 12V battery to 5V for ESP32

```
VIN+ → Battery POS
VIN- → Common GND
VOUT+ → MCU 5V pin
VOUT- → Common GND
```

**Output:** 5V @ up to 3A (set via onboard potentiometer)

### MCU (Heltec WiFi LoRa 32 v3 - ESP32-S3)

#### I2C Bus
```
GPIO1 → IC1 SDA, IC2 SDA (shared I2C data line)
GPIO2 → IC1 SCL, IC2 SCL (shared I2C clock line)
```

#### Analog Input
```
GPIO3 → Solar voltage divider midpoint (ADC input)
```

#### Digital Outputs (MOSFET Control)
```
GPIO4 → R3 → M1 Gate (PWM charge control)
GPIO48 → R5 → M2 Gate (load switch)
```

#### 1-Wire Bus
```
GPIO47 → DS18B20 DATA
```

#### Power
```
5V → LM2596 VOUT+
GND → Common GND
3.3V → IC1 VCC, IC2 VCC, DS18B20 VCC (powers sensors)
```

---

## Common Ground Rail Connections

All of the following connect to a common ground rail on the breadboard:

- Solar NEG (via M1 when ON)
- Battery NEG
- Load NEG (via M2 when ON)
- M1 gate pulldown (R4)
- M2 gate pulldown (R6)
- IC1 GND
- IC2 GND
- DS18B20 GND
- LM2596 VIN-
- LM2596 VOUT-
- MCU GND
- Voltage divider R2

---

## Current Flow Paths

### Charging Path (M1 ON)
```
Solar+ → IC1 VIN+
       → [IC1 shunt measures current]
       → IC1 VIN-
       → D1 Anode
       → D1 Cathode (Schottky diode forward conducts)
       → Battery+ (charges battery)

Solar- → M1 Drain
       → M1 Source (conducts when GPIO4 HIGH/PWM)
       → IC1 VIN+
       → Completes circuit
```

**Control:** GPIO4 PWM duty cycle controls charging current
**Protection:** D1 prevents reverse current when solar voltage < battery voltage

### Load Path (M2 ON)
```
Battery+ → IC2 VIN+
         → [IC2 shunt measures current]
         → IC2 VIN-
         → Load POS
         → Load NEG
         → M2 Drain
         → M2 Source (conducts when GPIO48 HIGH)
         → GND
```

**Control:** GPIO48 HIGH = load ON, LOW = load OFF

### Power for MCU
```
Battery+ → LM2596 VIN+
         → [Buck conversion 12V → 5V]
         → LM2596 VOUT+ (5V)
         → MCU 5V pin
         → [MCU internal regulator 5V → 3.3V]
         → MCU 3.3V rail
         → Powers: IC1, IC2, DS18B20, ESP32 logic
```

---

## GPIO Pin Assignments

| GPIO | Function | Direction | Description |
|------|----------|-----------|-------------|
| GPIO1 | I2C SDA | Bidirectional | I2C data for both INA219 sensors |
| GPIO2 | I2C SCL | Output | I2C clock for both INA219 sensors |
| GPIO3 | ADC | Input | Solar panel voltage (via divider) |
| GPIO4 | PWM | Output | M1 gate control (charge PWM) |
| GPIO48 | Digital Out | Output | M2 gate control (load switch) |
| GPIO47 | 1-Wire | Bidirectional | DS18B20 temperature sensor |

---

## Design Notes

### Why Low-Side Switching?
N-channel MOSFETs (like IRLZ44N) require the gate voltage to be higher than the source voltage by at least 2-4V to turn ON. When used on the high side (switching the positive rail), the source sits at battery voltage (~12V), requiring the gate to be at ~15-16V to turn ON. The ESP32 GPIO can only provide 3.3V, which is insufficient.

By switching the low side (negative/ground path), the MOSFET source is at 0V (ground), so a 3.3V gate signal provides Vgs = 3.3V, which is sufficient to fully turn ON the MOSFET.

### INA219 High-Side Sensing
Both INA219 sensors are configured for high-side current sensing, which measures current on the positive rail. This allows:
- Ground-referenced measurements (easier for microcontroller)
- Detection of shorts to ground
- No common-mode voltage issues

### Recent Improvements
1. **✓ Replaced M2 with Schottky diode (D1: 1N5822)**
   - Simpler reverse current protection
   - Freed up GPIO45
   - Trade-off: ~0.4V constant voltage drop vs. MOSFET's ~0.01V
   - No longer need to actively control reverse protection

2. **✓ Added pulldown resistor for M2 gate (R6: 10kΩ)**
   - Prevents floating gate when GPIO48 is uninitialized
   - Ensures MOSFET stays OFF during boot

### Future Improvements
1. **Add reverse polarity protection diode**
   - Schottky diode between battery and circuit
   - Prevents damage if battery connected backward

2. **Add fuse**
   - 3-5A fuse in series with battery positive
   - Protects against shorts and overcurrent

---

## Measured Values (Typical Operation)

### Solar Panel
- Open circuit voltage: 21.96V
- Operating voltage (charging): 13-15V
- Maximum current: 2A (25W panel)

### Battery (LiFePO4 12V 7Ah)
- Fully charged: 14.4V - 14.6V
- Nominal: 13.0V - 13.8V
- Discharged: 10.0V - 11.0V
- Charge rate: C/10 to C/3 (0.7A to 2.3A)

### Load (10W LED)
- Voltage: ~12V
- Current: ~0.83A
- Power: ~10W

### LM2596 Buck Converter
- Input: 12V (battery voltage)
- Output: 5.0V
- Efficiency: ~85%
- ESP32 consumption: 80-150mA typical

---

## Troubleshooting

### Issue: Solar voltage reads ~12V at night (should be 0V)
**Cause:** Battery voltage backfeeding through M1 body diode or MOSFET conducting when it shouldn't be.
**Solution:** Ensure GPIO4 is LOW when charging is disabled. D1 should prevent most backfeed, but body diode in M1 can still cause issues.

### Issue: Charge current reads 0A with sun
**Possible causes:**
1. M1 not turning ON (check GPIO4 is HIGH/PWM active)
2. PWM duty cycle set to 0%
3. D1 installed backward (anode should be on IC1 VIN- side)
4. INA219 damaged or wired incorrectly
5. Shunt resistor damaged or bypassed

### Issue: Board stays powered with battery disconnected
**Possible causes:**
1. USB cable connected (powers via USB)
2. Parasitic power through IC1/IC2 protection diodes
3. M1 conducting when it should be OFF
4. Ground loop or shared power rail issue

### Issue: ADC reading incorrect voltage
**Possible causes:**
1. 5V to GND short (check for solder bridges!)
2. Wrong voltage divider ratio in code
3. Incorrect ADC attenuation setting
4. ESP32 ADC needs calibration

## Author

Copyright 2025, Michael Green
