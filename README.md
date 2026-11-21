# esp32-ld2410-presence-radar-mqtt — publish mmWave presence and distance from an LD2410 radar module over MQTT

IoT example by [Fluidwire](https://fluidwire.com/).

Sample sketch for an ESP32 dev board that reads a **Hi-Link HLK-LD2410 24 GHz mmWave radar** over UART and publishes human **presence** to MQTT — including people who are sitting perfectly still.

A PIR sensor only sees *motion*, so a room with someone reading at a desk times out to "empty" after a minute or two. The LD2410 tracks a **stationary** target as well as a moving one, which is what makes it the right occupancy sensor for study rooms, offices, bedrooms and bathroom lights. This sketch exposes both, plus distance and signal energy, so your automation can tell "walked in" from "still here".

## What it does

The ESP32 listens to the LD2410's UART data frames (one every ~100 ms) with the `ld2410` library. The moment the radar reports a moving **or** stationary target, the sketch publishes a retained `occupied` to the state topic and a JSON event. It only publishes `clear` after `CLEAR_HOLD_MS` (default 5 s) with no target at all, so a brief dropout does not flap the state. Every `TELEMETRY_MS` (default 5 s) a full telemetry frame goes out with detected/distance/energy for both the moving and stationary target. If the radar stops sending frames for 10 s the sketch warns on Serial and reports `"radar":"offline"` so the dashboard shows the sensor is gone rather than silently "clear".

## Required hardware

- 1x ESP32 dev board
- 1x HLK-LD2410 (or LD2410B / LD2410C) mmWave radar module
- Jumper wires (the module has 1.27 mm pitch pads; most sellers include a pigtail)
- An MQTT broker (local Mosquitto, or a public test broker like `broker.hivemq.com`)
- A 2.4 GHz WiFi network

## Required libraries

Install via Library Manager:

- **ld2410** by Nick Reynolds
- **PubSubClient** by Nick O'Leary

## Wiring

| LD2410 pin | ESP32 pin        |
|------------|------------------|
| VCC        | 5V (VIN)         |
| GND        | GND              |
| TX         | GPIO16 (UART2 RX)|
| RX         | GPIO17 (UART2 TX)|
| OUT        | not used         |

The module **must be powered from 5 V** (it draws ~80 mA and the on-board regulator needs the headroom), but its UART pins are 3.3 V logic, so they connect straight to the ESP32 — no level shifter. TX goes to RX and RX goes to TX. The default baud rate is **256000**; if you changed it with the HLKRadarTool phone app, update `RADAR_BAUD` in the sketch.

Mount the module with the antenna side facing the area you want to cover, ideally 1.5–2 m high and angled slightly down. Keep it away from fans, curtains and aquariums — anything that moves looks like a person to a radar.

## Setup

1. Copy `config.h.example` → `config.h`. Fill in your WiFi + MQTT broker details and (optionally) change the topics.
2. Wire the LD2410 as above.
3. Flash. Open the Serial Monitor at **115200 baud**.

## Expected serial output

```
[boot] ESP32 LD2410 presence radar -> MQTT
[radar] starting LD2410 on UART2... ok
[radar] firmware v2.4.22062416
[wifi] connecting to my-network
....
[wifi] IP: 192.168.1.42
[mqtt] connecting... ok
[mqtt] {"presence":false,"moving":{"detected":false,"distance_cm":0,"energy":0},"stationary":{"detected":false,"distance_cm":0,"energy":0},"uptime_s":4}
[mqtt] {"event":"occupied","moving":true,"stationary":false,"distance_cm":182,"uptime_s":41}
[mqtt] {"presence":true,"moving":{"detected":true,"distance_cm":182,"energy":67},"stationary":{"detected":true,"distance_cm":190,"energy":41},"uptime_s":45}
[mqtt] {"event":"clear","moving":false,"stationary":false,"distance_cm":0,"uptime_s":133}
```

Watch it from your machine:

```
mosquitto_sub -h broker.hivemq.com -v -t "fluidwire/demo/presence/#"
```

The `state` topic is **retained**, so a dashboard or Home Assistant that connects later immediately gets `occupied` or `clear` without waiting for the next change.

## Troubleshooting

- **`[radar] starting LD2410... no response`** — TX/RX are swapped, or the module is on 3V3 instead of 5V. Also confirm the baud rate: factory default is 256000, but some batches ship at 115200.
- **Always `occupied`, never `clear`** — radar sees something moving: a fan, a curtain, a pet, or yourself through a thin wall. Point the module away from it, or lower the stationary sensitivity / max gate with the HLKRadarTool app (the sketch reads whatever the module is configured to report).
- **`clear` comes too fast when someone sits still** — the module's own "unmanned duration" setting is short. Raise it in HLKRadarTool, and/or raise `CLEAR_HOLD_MS` in the sketch.
- **Distance jumps around** — normal for a radar at the edge of a gate (each gate is 75 cm). Use `energy` as a confidence value and smooth distance on the receiving side.
- **`failed state=-2`** on MQTT — the broker host/port is wrong or unreachable. `-2` is a network/connection failure.
- **Garbage on Serial Monitor** — you are at the wrong baud: the USB console is 115200, the radar UART is 256000.

## Security note

This sketch keeps your WiFi and MQTT credentials in `config.h`, which is `.gitignore`'d — it never gets committed. Only `config.h.example` ships in the repo. Never hardcode real SSID/password into the `.ino`. Presence data reveals when a room (or home) is occupied, so on anything beyond a bench test use a private broker with `MQTT_USER`/`MQTT_PASSWORD` (and TLS — see [esp32-mqtt-tls-secure](https://github.com/fluidwire/esp32-mqtt-tls-secure)) rather than a public one.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — happy to help.
