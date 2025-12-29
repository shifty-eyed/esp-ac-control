## ESP32 AC Control – WiFi HTTP API + Thermostat Interface

This project controls an AC unit via an **ESP32-WROOM-32** board connected to a **proprietary wall thermostat**.  
The ESP32 exposes a **simple HTTP API** over WiFi and electrically **emulates the thermostat’s ON/OFF button** and **reads the thermostat’s LED status**.

- **HTTP API (port 80)**:
  - **`GET /status`** → returns JSON with AC status, current time, schedules, and webhook URL.
  - **`PUT /on`** → emulates a button press only if AC is currently OFF.
  - **`PUT /off`** → emulates a button press only if AC is currently ON.
  - **`PUT /ha-hook-url?value=<url>`** → configure Home Assistant webhook URL.
  - **`GET /time`** → returns current system time and sync status (JSON).
  - **`PUT /synctime`** → manually trigger NTP time synchronization.
  - **`GET /schedule`** → list all configured schedules (JSON).
  - **`PUT /schedule`** → create or update a schedule.
  - **`DELETE /schedule`** → delete a schedule by ID.
  - **`GET /journal`** → retrieve activity log with timestamps.
  - **`DELETE /journal`** → clear activity log.
  - **`GET /state-journal`** → retrieve AC state change history (CSV format).
  - **`DELETE /state-journal`** → clear state change history.

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
- **One relay module** (e.g., 1-channel 5V relay module) to emulate the button press.
- Connection to thermostat:
  - **Two button wires** (momentary ON/OFF) - connected via relay contacts.
  - **Two LED wires** (indicating ON/OFF state) - connected to ESP32 GPIO for sensing.

> **Important:** There is **no galvanic isolation** between ESP32 and thermostat in this design.
> The relay provides electrical isolation for the button control, but the LED sense line shares a common ground.
> You accept the risk that sharing ground and interfacing directly may stress the thermostat electronics if its internal design is unusual.

---

## ESP32 Pin Assignment

- **Relay control output (ACTIVE LOW)**:
  - **`GPIO25`** – output (LOW = relay ON = button pressed, HIGH = relay OFF = button released)
- **Thermostat LED sense input (digital)**:
  - **`GPIO32`** – digital input (reads ~3V when AC is ON, ~0V when AC is OFF)

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

## Wiring – Button Emulation with Relay Module

Goal: When ESP32 `GPIO25` goes **LOW**, the relay activates and **closes the button contacts** (in parallel with the physical button), emulating a button press.

### 1. Identify button ground and signal

1. Power the thermostat as normal.
2. With a multimeter:
   - Black probe on the **LED negative** (this is thermostat ground).
   - Red probe on each of the two **button wires**.
3. You should see:
   - One button wire at ≈ **0 V** → this is **button ground side**.
   - The other at some positive voltage (around **5 V**) or varying when pressed → **button signal side**.

Keep the physical button connected; we simply add the relay contacts **in parallel**.

### 2. Relay module wiring

Use a standard **1-channel 5V relay module** (commonly available with opto-isolated input).

**Relay Module Connections:**

- **VCC** (relay module power):
  - Connect to **ESP32 5V** (VIN pin on dev board, powered from USB).

- **GND** (relay module ground):
  - Connect to **ESP32 GND**.

- **IN** (relay control signal):
  - Connect to **ESP32 `GPIO25`**.
  - Most relay modules are **ACTIVE LOW**: when GPIO25 is LOW, relay activates.

**Relay Contacts (to thermostat button):**

- **COM** (common terminal):
  - Connect to **button ground side** (the wire that measured ~0V).

- **NO** (normally open terminal):
  - Connect to **button signal side** (the wire that measured ~5V).

**Shared Ground:**

- Connect **thermostat ground** (LED negative wire) to **ESP32 GND**.
  - This common ground reference is necessary for the LED sense circuit to work correctly.

### How it works

- When `GPIO25` is **HIGH** → relay is **off**, button contacts are **open**, button behaves normally.
- When `GPIO25` is **LOW** → relay **activates**, NO contact **closes** to COM, **shorting the button signal to ground**, emulating a button press.

The ESP32 code drives GPIO25 LOW for 300ms to emulate a button press, then returns it HIGH to release.

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

```bash
# Check AC status (returns JSON with status, time, schedules, webhook URL)
curl http://<esp-ip>/status

# Turn AC on
curl -X PUT http://<esp-ip>/on

# Turn AC off
curl -X PUT http://<esp-ip>/off

# Configure Home Assistant webhook
curl -X PUT "http://<esp-ip>/ha-hook-url?value=http://<ha-ip>:8123/api/webhook/<webhook-id>"

# Create/update a schedule (turn AC on at 7:30 AM)
curl -X PUT "http://<esp-ip>/schedule?id=0&hour=7&minute=30&switch=1"

# Delete a schedule
curl -X DELETE "http://<esp-ip>/schedule?id=0"

# View activity journal
curl http://<esp-ip>/journal

# View state change history (CSV)
curl http://<esp-ip>/state-journal

# Clear journals
curl -X DELETE http://<esp-ip>/journal
curl -X DELETE http://<esp-ip>/state-journal
```

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
curl -X PUT "http://192.168.4.120/schedule?id=0&hour=7&minute=30&switch=1"

# Turn AC OFF at 10:00 PM (22:00)
curl -X PUT "http://192.168.4.120/schedule?id=1&hour=22&minute=0&switch=0"



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

## Home Assistant Integration

The system includes **webhook integration** to automatically notify Home Assistant whenever the AC state changes.

### Configuring the Webhook

Set the Home Assistant webhook URL using the `/ha-hook-url` endpoint:

```bash
curl -X PUT "http://<esp-ip>/ha-hook-url?value=http://<ha-ip>:8123/api/webhook/<webhook-id>"
```

**Example:**
```bash
curl -X PUT "http://192.168.4.120/ha-hook-url?value=http://192.168.4.199:8123/api/webhook/ac_status_main"
```

The webhook URL is stored persistently in NVS and survives ESP32 reboots.

### Webhook Behavior

The ESP32 automatically sends an HTTP POST request to the configured webhook URL whenever the AC state changes:

- **When triggered:**
  - Manual API control via `/on` or `/off` endpoints
  - Scheduled events (when a schedule triggers a state change)
  - External state changes detected (someone uses the physical thermostat button)

- **Payload format (JSON):**
  ```json
  {"status":"on"}
  ```
  or
  ```json
  {"status":"off"}
  ```

- **HTTP timeout:** 5 seconds
- **Logging:** All webhook attempts are logged to the activity journal with HTTP response codes

### Checking Current Configuration

The configured webhook URL is included in the `/status` endpoint response:

```bash
curl http://<esp-ip>/status
```

Response includes:
```json
{
  "status": "1",
  "time": "2025-12-29 10:30:00",
  "schedules": [...],
  "ha_webhook": "http://192.168.4.199:8123/api/webhook/ac_status_main"
}
```

---

## External State Change Detection

The system continuously monitors the AC state to detect **external changes** that occur outside of the ESP32's control, such as someone manually pressing the physical thermostat button.

### How It Works

- **Monitoring interval:** Every 5 seconds
- **Detection method:** Compares current AC state (read from LED) against the last known state
- **Actions when external change detected:**
  1. Updates the internal state tracker
  2. Logs the state change to the state journal
  3. Sends a webhook notification to Home Assistant (if configured)

### Use Cases

- **Track manual overrides:** Know when someone uses the physical thermostat instead of the API
- **Synchronize state:** Keep Home Assistant and other systems synchronized even when the thermostat is manually controlled
- **Debugging:** Identify unexpected state changes or thermostat behavior
- **Analytics:** Build a complete history of all AC state changes regardless of source

### Example

If you turn on the AC using `/on`, the system knows it initiated the change. But if someone walks up and presses the physical button, the external state change detection will:
- Detect the change within 5 seconds
- Log it: `[2025-12-29 14:32:15] External state change detected`
- Record the timestamp and new state in the state journal
- Notify Home Assistant via webhook

This ensures complete visibility into all AC state changes, whether controlled by the API, schedules, or manual button presses.

---

## Activity Logging (Journal System)

The system includes two types of journals for tracking activity and debugging:

### 1. Activity Journal

A **circular buffer** that stores the most recent **200 events** with timestamps.

**What it logs:**
- Manual API requests (`/on`, `/off`)
- Schedule triggers and results
- Webhook POST attempts and HTTP response codes
- Configuration changes (webhook URL updates)
- External state changes detected

**Format:**
```
[YYYY-MM-DD HH:MM:SS] message
```

**Example output:**
```
[2025-12-29 07:30:00] Schedule #0 triggered: Turn ON
[2025-12-29 07:30:01] Schedule #0 result: Success from 0 retry
[2025-12-29 07:30:02] HA webhook: on -> HTTP 200
[2025-12-29 14:15:23] Manual turn OFF requested
[2025-12-29 14:15:24] Manual turn OFF result: Already there
```

**API Endpoints:**

```bash
# View activity journal
curl http://<esp-ip>/journal

# Clear activity journal
curl -X DELETE http://<esp-ip>/journal
```

### 2. State Journal

A **linear buffer** that stores up to **3000 state change records** for long-term analytics.

**What it logs:**
- Every AC state transition (ON→OFF or OFF→ON)
- Includes both API-initiated and externally-detected changes

**Format (CSV):**
```
timestamp,state
```

Where:
- `timestamp` = Unix timestamp (seconds since epoch)
- `state` = `1` (AC on) or `0` (AC off)

**Example output:**
```
1735473000,1
1735480200,0
1735494600,1
```

**API Endpoints:**

```bash
# View state journal (CSV format)
curl http://<esp-ip>/state-journal

# Clear state journal
curl -X DELETE http://<esp-ip>/state-journal
```

### Use Cases

- **Debugging:** Trace exactly what happened and when
- **Analytics:** Export state journal CSV for analysis (uptime statistics, usage patterns, etc.)
- **Monitoring:** Check recent activity via journal endpoint
- **Troubleshooting:** Identify why a schedule didn't trigger or why a state change failed

The activity journal is useful for real-time debugging with human-readable messages, while the state journal provides raw data perfect for importing into spreadsheets or data analysis tools.

---

## Safety Notes

- Double-check **voltages** with a multimeter before final wiring.
- Verify that the **thermostat ground point** you tie to ESP32 GND is correct and stable.
- Start with **short test pulses** and monitor for any unusual thermostat behavior (reboots, flicker, etc.).
- If at any point the thermostat behaves erratically, consider switching to a **relay or optocoupler-based design** for full isolation.

---

With this wiring and setup, you get a **simple WiFi-controlled AC switch** that mirrors the existing wall thermostat’s logic and state via a clean HTTP API.


