# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid
3. **New-aircraft card** — when a plane appears, a full-screen card shows its airline logo, flight, route, aircraft and live stats (see [New-aircraft card](#new-aircraft-card))
4. **Web portal** — settings, firmware updates over Wi‑Fi, and a live log page

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~5 s).

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Dismiss the new-aircraft card if one is showing; otherwise cycle range preset (5 → 10 → 15 → 25 km, also leaves a custom range); saved to flash |
| **Hold 3 s** | Clear Wi‑Fi, location, and units; reboot into setup portal |

During setup you can also hold BOOT at power-on to force a credential reset (same as the long press).

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. **Configure WiFi** to change networks; **Setup** for location, range, units, icon size, runways and card duration; save

Changes apply immediately, no reboot.

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |
| **Radar range** | km shown on ring 3, any whole number 2–50 (BOOT tap returns to the presets) |
| **Aircraft icon size** | 1 = small (original footprint), 2 = medium (default), 3 = large |
| **New-aircraft card duration** | Seconds the card stays up, 0–60 (0 = cards off; default 10) |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

### Firmware updates over Wi‑Fi (OTA)

**Update** in the portal menu uploads a new firmware: choose **`.pio/build/supermini/firmware.bin`** (the app image, **not** the merged `0x0` image). The radar shows *Updating firmware…* and pauses drawing and ADS-B fetches while the upload runs (~70 s), then reboots into the new build. A failed upload leaves the running firmware untouched and the radar resumes.

OTA needs the two-app-slot layout in `partitions/plane_radar.csv`. **Upgrading from a release with the old single-slot layout needs one USB flash.** To keep saved Wi‑Fi and settings, write only these (NVS at `0x9000` is untouched):

```bash
esptool.py --chip esp32c3 write_flash \
  0x8000  .pio/build/supermini/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/supermini/firmware.bin
```

Flashing the merged image at `0x0` also works but erases saved settings.

### Log page

**`http://plane-radar.local/log`** shows the last 50 events (new aircraft, lookups, logo source, HTTP/TLS errors, Wi‑Fi drops) with uptime stamps, plus free heap, largest free block and Wi‑Fi RSSI. Useful for debugging without a USB cable.

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — red top-view plane silhouette rotated to heading (size 1–3 from **Setup**), magenta speed vector (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

## New-aircraft card

When an aircraft that hasn't been seen for 30 minutes enters the ADS-B fetch area, the closest one gets a full-screen card for the configured duration (default 10 s). Planes already overhead at boot don't trigger a card, and newcomers wait for the current card to finish instead of cutting it short.

| Card element | Source |
|--------------|--------|
| Airline logo | FlightAware logos ([Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos)); see below |
| Flight number, airline, route (airports + cities) | [adsbdb.com](https://www.adsbdb.com/) by callsign |
| Aircraft model, registration | adsbdb.com by ICAO hex |
| Route progress line | Live position vs. origin/destination coordinates |
| Altitude, speed, distance + direction | Live ADS-B, updated while the card is up |

- **Countdown ring** around the edge shows time left; **BOOT tap** dismisses the card.
- **Stale routes are hidden.** adsbdb routes come from a callsign database and can be out of date (flight numbers get reused). If going via the plane's position would add more than ~30% to the route, the card shows *Route not available* instead.
- **No route data** (private / GA traffic): callsign, type and live stats only; the badge shows the airline code or a plane icon when there's no logo.

### Logos

The C3 has no PSRAM and can't spare the ~43 KB contiguous a PNG decode needs, so logos are 34 px JPEG pairs (composited on black and on white; the difference gives transparency):

1. **Bundled in flash** — the 20 most frequent airlines at Delhi (DEL), generated by `scripts/build_airline_logos.py`. Regenerate for your area: `python3 scripts/build_airline_logos.py IGO AIC …` (airline ICAO codes), then rebuild.
2. **Downloaded** — other airlines via the [wsrv.nl](https://wsrv.nl/) image proxy (resizes and converts FlightAware's PNG to JPEG), then kept in a 5-slot RAM cache.

Dark logos (e.g. Lufthansa, IndiGo) get a white disc so they stay visible on black (`kLogoDarkLuminance` in `config.h`); square tile logos get rounded corners. Card colors and logo pixels are pre-swapped for the panel's R/B order (`kDisplayRgbOrder`).

### Fonts

Card and radar text use **Barlow Semi Condensed** (SIL OFL, `data/fonts/OFL.txt`), pre-rendered to VLW at exact pixel sizes. Regenerate with `scripts/make_vlw.py` (`pip install freetype-py`).

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |
| Info card | `kFlightCardDefaultSec`, `kFlightCardMaxSec`, `kFlightCardForgetMs` (when a plane counts as new again), `kLogoDarkLuminance` |

Range presets: `include/ui/radar_range.h` (`kRangePresets`). Range, icon size and card duration can also be changed at runtime from the portal's **Setup** page.

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
    airline_logos.h        — bundled logo table
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h          — ADS-B fetch, adsbdb lookups, logo cache
    log_buffer.h           — /log page ring buffer
data/
  ui_font.vlw              — radar UI font (Barlow Semi Condensed)
  fonts/                   — info card fonts + OFL license
scripts/
  build_large_airports.py
  build_airline_logos.py   — bundle frequent airlines' logos
  make_vlw.py              — TTF → VLW font converter
src/
  main.cpp
  data/
    large_airports_data.cpp
    airline_logos_data.cpp — generated
  hardware/
  ui/
  services/
```

## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **10** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push / PR to `main` | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256` |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches the merged image to the release. Download from **Releases** on GitHub, then flash at **0x0** (ESP32-C3, 4 MB).

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

Data and assets (fetched at runtime or bundled):

- [adsb.fi](https://opendata.adsb.fi/) — live ADS-B
- [adsbdb](https://www.adsbdb.com/) — routes and aircraft details
- [Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos) — FlightAware airline logos (trademarks of their airlines), via [wsrv.nl](https://wsrv.nl/)
- [vradarserver/standing-data](https://github.com/vradarserver/standing-data) — used to pick the bundled airlines
- [Barlow](https://github.com/jpt/barlow) — font, SIL Open Font License
