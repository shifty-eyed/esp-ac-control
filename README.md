## ESP32 AC Control – WiFi HTTP API + Thermostat Interface

This project controls an AC unit via an **ESP32-WROOM-32** board connected to a **proprietary wall thermostat**.  
The ESP32 exposes a **simple HTTP API** over WiFi and electrically **emulates the thermostat’s ON/OFF button** and **reads the thermostat’s LED status**.

- **HTTP API (port 80)**:
  - **`GET /status`** → returns `"1"` if AC is ON, `"0"` if AC is OFF (based on the thermostat LED).
  - **`PUT /on`** → emulates a button press only if AC is currently OFF.
  - **`PUT /off`** → emulates a button press only if AC is currently ON.
  - **`GET /time`** → returns current system time and sync status (JSON).
  - **`PUT /synctime`** → manually trigger NTP time synchronization.
  - **`GET /schedule`** → list all configured schedules (JSON).
  - **`PUT /schedule`** → create or update a schedule.
  - **`DELETE /schedule`** → delete a schedule by ID.

The project assumes:

- Thermostat **button voltage: 5 V DC**.
- Thermostat **LED sense node: ~0 V off, ~3 V on (DC, measured to thermostat ground)**.
- ESP32 is powered from a **separate 5 V USB power supply**, but **shares ground** with the thermostat electronics.

---

## Framework Choice

The recommended stack for this project is:

- **PlatformIO** (as the build system/IDE).
- **Arduino framework for ESP32**.

**Why Arduino + PlatformIO:**

- Very quick to bring up **WiFi** and an **HTTP server**.
- Lots of reference code and simple APIs (`WiFi.h`, `WebServer.h`).
- Good enough for this relatively small project.

You can later migrate to **ESP-IDF** if you need more control, but Arduino is ideal to get this working fast.

---

## Hardware Overview

- **ESP32 dev board** (e.g. ESP32-WROOM-32 DevKitC).
- **Separate USB 5 V supply** powering the ESP32 dev board.
- Connection to thermostat:
  - **Two button wires** (momentary ON/OFF).
  - **Two LED wires** (indicating ON/OFF state).
- **One NPN transistor + a few resistors** to emulate the button press.

> **Important:** There is **no galvanic isolation** in this design.  
> You accept the risk that sharing ground and interfacing directly may stress the thermostat electronics if its internal design is unusual.

---

## Exact Transistor Options

Use **one small-signal NPN BJT** to emulate the button press.  
Any of the following common parts are suitable:

- **2N2222A** (or PN2222A)
- **BC547B**
- **2N3904**

All of these are cheap, widely available, and can handle the tiny current used by a 5 V thermostat button line.

> **Note:** Pinouts vary by package and manufacturer.  
> Always check the **datasheet** and confirm which pins are **E (Emitter), B (Base), C (Collector)** before wiring.

In this README we refer to generic **E / B / C**, independent of the exact package orientation.

---

## ESP32 Pin Assignment

- **Button driver output (to transistor base)**:  
  - **`GPIO25`** – output
- **Thermostat LED sense input (digital)**:  
  - **`GPIO32`** – digital input

These pins avoid boot-strapping issues and are safe, general-purpose choices on ESP32-WROOM-32.

---

## Power and Grounding

- **ESP32 power:**
  - 5 V from **USB power supply** into ESP32 dev board.
  - The board’s onboard regulator provides **3.3 V** for the ESP32 and GPIO.
- **Thermostat power:**
  - Remains completely on its **own internal supply**.
- **Shared ground (required):**
  - Connect **ESP32 GND** to **thermostat ground**:
    - The thermostat ground is the **LED negative** line and usually one side of the button.

This common ground allows ESP32 GPIO voltages to be correctly referenced to the thermostat signals.

---

## Wiring – Button Emulation with NPN Transistor

Goal: When a specific ESP32 GPIO goes HIGH, the transistor conducts and **shorts the button contacts** (in parallel with the real button), emulating a press.

### 1. Identify button ground and signal

1. Power the thermostat as normal.
2. With a multimeter:
   - Black probe on the **LED negative** (this is thermostat ground).
   - Red probe on each of the two **button wires**.
3. You should see:
   - One button wire at ≈ **0 V** → this is **button ground side**.
   - The other at some positive voltage (around **5 V**) or varying when pressed → **button signal side**.

Keep the physical button connected; we simply add the transistor **in parallel**.

### 2. Transistor wiring

Use any of the specified NPNs (2N2222A, BC547B, 2N3904).

- **Emitter (E)**:
  - Connect to **thermostat ground**.
  - Tie to **ESP32 GND** (shared ground).

- **Collector (C)**:
  - Connect to the **button signal side** (the non-ground button wire).

- **Base (B)**:
  - Connect **ESP32 `GPIO25`** to base **through a 4.7 kΩ resistor**.
  - Add a **100 kΩ resistor from base to ground** (thermostat/ESP shared ground).

In summary:

- `GPIO25` → **4.7 kΩ** → B
- B → **100 kΩ** → GND
- E → GND (ESP32 + thermostat)
- C → button signal line

When `GPIO25` is LOW → transistor **off**, button behaves normally.  
When `GPIO25` is HIGH (3.3 V) → base current ≈ (3.3 − 0.7) / 4.7k ≈ 0.55 mA, transistor **saturates** and **shorts signal to ground**, emulating a press.

---

## Wiring – LED Status to ESP32 (Digital Input)

You are tapping the LED node which measures:

- **~0 V when OFF**
- **~3 V when ON**, relative to thermostat ground

This is below 3.3 V, so we can safely feed it into an ESP32 **digital input** with a small series resistor.

### LED sense wiring (digital)

- Choose **`GPIO32`** as digital input.
- Connections:
  - Thermostat **LED_SENSE node** (~3 V when ON, 0 V when OFF) → **10 kΩ resistor** → **`GPIO32`**.
  - Thermostat **GND** → **ESP32 GND** (shared).
  - *Optional:* Place a **100 nF capacitor** from `GPIO32` to GND (near the ESP32) to reduce noise.

The 10 kΩ resistor limits current and provides protection for the GPIO pin.

### Software – digital read

In Arduino-style code:

```cpp
const int LED_SENSE_PIN = 32;  // digital input

void setup() {
  pinMode(LED_SENSE_PIN, INPUT);
}

bool isAcOn() {
  return digitalRead(LED_SENSE_PIN) == HIGH;  // HIGH ≈ LED ON (3 V)
}
```

---

## Software Sketch Outline (Arduino + PlatformIO)

This section intentionally left open so you can keep the software implementation in code files  
(`main.cpp`, etc.) rather than embedding it in the README.

---

## PlatformIO Quick Start

1. **Create project** in PlatformIO:
   - Board: **`esp32dev`** (or your specific ESP32 DevKit).
   - Framework: **Arduino**.
2. Implement your logic in `src/main.cpp` using the wiring and API description above.
3. Set your **WiFi credentials** in your code (`ssid` / `password` or similar).
4. Connect the ESP32 via USB.
5. In PlatformIO:
   - **Build** → **Upload** → **Monitor**.
6. Once running, note the **IP address** printed over serial.

Test from a PC on the same network:

- `curl http://<esp-ip>/status`
- `curl -X PUT http://<esp-ip>/on`
- `curl -X PUT http://<esp-ip>/off`

---

## Time Synchronization and Scheduling

The system includes **NTP time synchronization** and **persistent schedule management** to automatically control the AC at specific times.

### Time Synchronization

- **NTP Server**: `pool.ntp.org`
- **Timezone**: GMT-5 (Eastern US) – configurable in code
- **Sync Mode**: Manual only (no automatic re-sync)
- Initial sync attempted on boot; device continues to work even if sync fails

#### Time API Endpoints

**GET /time** – Get current system time

```bash
curl http://<esp-ip>/time
```

Response:
```json
{"time": "2025-11-25 14:30:00", "synced": true}
```

**PUT /synctime** – Manually trigger NTP sync

```bash
curl -X PUT http://<esp-ip>/synctime
```

Response:
```json
{"status": "syncing"}
```

### Schedule Management

Up to **16 schedules** (IDs 0-15) are stored persistently in **NVS (Non-Volatile Storage)** and survive ESP32 reboots.

Each schedule specifies:
- **id**: Schedule slot (0-15)
- **hour**: Hour of day (0-23)
- **minute**: Minute of hour (0-59)
- **switch**: Desired AC state (1 = turn on, 0 = turn off)

The system checks schedules every loop iteration and automatically triggers the AC when the scheduled time is reached.

#### Schedule API Endpoints

**GET /schedule** – List all schedules

```bash
curl http://<esp-ip>/schedule
```

Response:
```json
[
  {"id": 0, "hour": 7, "minute": 30, "switch": 1},
  {"id": 1, "hour": 22, "minute": 0, "switch": 0}
]
```

**PUT /schedule** – Create or update a schedule

```bash
# Turn AC ON at 7:30 AM
curl -X PUT "http://<esp-ip>/schedule?id=0&hour=7&minute=30&switch=1"

# Turn AC OFF at 10:00 PM (22:00)
curl -X PUT "http://<esp-ip>/schedule?id=1&hour=22&minute=0&switch=0"
```

Response:
```json
{"status": "ok", "id": 0}
```

**DELETE /schedule** – Delete a schedule

```bash
curl -X DELETE "http://<esp-ip>/schedule?id=0"
```

Response:
```json
{"status": "deleted", "id": 0}
```

#### Parameter Validation

- **id**: Must be 0-15
- **hour**: Must be 0-23 (24-hour format)
- **minute**: Must be 0-59
- **switch**: Must be 0 (off) or 1 (on)

Invalid parameters return HTTP 400 with error details in JSON.

### Schedule Behavior

- Schedules are checked every loop iteration (≈2ms)
- When a schedule time is reached, the system:
  1. Checks current AC state
  2. If AC is already in desired state, no action is taken
  3. If AC needs to change state, a button press is emulated
- Each schedule triggers only once per minute (prevents duplicate execution)
- Schedules persist through reboots via NVS storage

---

## Safety Notes

- Double-check **voltages** with a multimeter before final wiring.
- Verify that the **thermostat ground point** you tie to ESP32 GND is correct and stable.
- Start with **short test pulses** and monitor for any unusual thermostat behavior (reboots, flicker, etc.).
- If at any point the thermostat behaves erratically, consider switching to a **relay or optocoupler-based design** for full isolation.

---

With this wiring and setup, you get a **simple WiFi-controlled AC switch** that mirrors the existing wall thermostat’s logic and state via a clean HTTP API.


