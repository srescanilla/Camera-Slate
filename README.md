# E-Paper Camera Slate

A small battery-powered slate for film sets. It shows, on an e-paper display that
stays readable in daylight and holds its image with the power off:

- Camera ID (A, B, C…)
- Lens, with its T-stop
- Frame rate and shutter
- Up to three filters, one per matte-box slot

Each shoot uses a different set of lenses and filters, so the device does not
scroll through a huge built-in catalogue. Before the shoot you load that day's
short lists from a phone over Bluetooth; on set you switch between them with the
rotary switch, without touching the phone.

---

## Hardware

| Part | Notes |
|---|---|
| Elecrow CrowPanel E-Paper HMI 4.2" (ESP32-S3-WROOM-1-N8R8) | Display, MCU, rotary switch and MENU/EXIT buttons all on one board |
| LiPo battery, SH1.0 2-pin connector | The board charges it over USB-C |
| RGB LED + 3 × 220 Ω resistors | Optional. Shows the camera colour at a glance |
| 3D-printed enclosure | Not included here |

The LED is wired to IO15 (R), IO16 (G) and IO17 (B). Everything else is already
wired on the board.

### ⚠️ Two hardware revisions exist under the same product name

This is the single most important thing in this repository.

The 4.2" CrowPanel has shipped with **two different display controllers**:

- **Older revision** (no sticker): SSD1683. Works with the GxEPD2 library.
- **Newer revision** (a **green round sticker on the back**): a different
  controller. It needs **GPIO41 held HIGH** as a second power enable, and it does
  **not** work with any GxEPD2 driver class.

Elecrow's own manual says SSD1683 for both. If you use GxEPD2 on the newer
revision, every refresh ends in `Busy Timeout!` and the screen never changes,
with no hint as to why.

**This firmware targets the newer (green sticker) revision.** If you have the
older one, this code will not drive your panel.

---

## Building the firmware

Arduino IDE, with the **esp32** board package by Espressif installed.

Board: **ESP32S3 Dev Module**. Then, under Tools:

| Setting | Value |
|---|---|
| USB CDC On Boot | Disabled |
| Flash Mode | QIO 80MHz |
| Flash Size | 8MB (64Mb) |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| PSRAM | OPI PSRAM |
| Upload Speed | 115200 |

Library required: **Adafruit GFX Library**. GxEPD2 is *not* used.

Arduino IDE 2.x remembers the board per sketch window, so check these again
whenever you open the file in a new window.

Serial output is 115200 baud. `USB CDC On Boot` must be **Disabled** for it to
appear, because this board's USB-C port goes through a USB-serial bridge rather
than the ESP32-S3's native USB.

---

## Controls

**Main screen**

| Input | Action |
|---|---|
| Rotary up/down | Move between fields (or change the value while editing) |
| Rotary press | Enter edit mode / save and leave |
| MENU (short) | Open the list of options for the selected field |
| MENU (held) | Device status screen |
| EXIT | Cancel the edit, or clear the selection |

**List screen**

| Input | Action |
|---|---|
| Rotary up/down | Move the cursor through the grid |
| Rotary press | Pick the value and go back |
| EXIT / MENU | Go back without changing anything |

Picking a filter that is already in another slot swaps the two slots.

---

## The phone page

`index.html` is a single self-contained page. It uses **Web Bluetooth**, which
means two hard requirements:

1. **It must be served over HTTPS.** Opening the file from the phone's
   Downloads folder will not connect. GitHub Pages is enough.
2. **iOS Safari does not support Web Bluetooth in any version.** On iPhone, open
   the page in an app such as Bluefy. On Android, Chrome works.

The page warns you about both of these if it detects them.

What it does:

- Keeps a catalogue that grows as you type new lenses and filters
- Saves named sets of lenses and filters to reuse on later shoots
- Ticks off which frame rates and shutters to load
- Exports and imports the whole thing as a JSON file
- Shows what the device is currently displaying

**The catalogue lives in the browser's local storage.** It is lost if you clear
browsing data, change phones, or open the page in another browser. Export a file
periodically; that is the real backup.

### Device limits

| List | Max entries |
|---|---|
| Lenses | 12 |
| Filters | 24 |
| Frame rates | 8 |
| Shutters | 10 |

Names are limited to **19 characters**, and to the basic ASCII range, because the
display fonts contain nothing else. `BPM 1/4` and `BLACK PRO-MIST 1/4` are fine;
a single-glyph `¼` is not. The page warns before dropping anything.

---

## How the lists are transferred

One list entry per Bluetooth write, so every packet fits inside the smallest MTU
any phone is guaranteed to offer. Roughly 54 writes, about two seconds.

Characteristic `6a2f1000-000a-…`:

| Bytes | Meaning |
|---|---|
| `F0` | Begin: copy the live lists into a staging area |
| `[type][index][text…]` | Store one entry in staging |
| `FF [type] [count]` | Set how many entries this list has |
| `FE` | Commit: staging becomes live and is saved |
| `F1 [type] [index]` | Stage one entry for reading back |

Types: `0` lenses, `1` filters, `2` frame rates, `3` shutters.

Nothing on the device changes until the commit, so a connection dropped
mid-transfer leaves the old lists intact.

Values are stored as **text, not as list positions**. Loading a new set of lists
never silently changes what the slate is displaying.

---

## Display refresh

The panel supports two waveforms. Moving around uses the fast one (no flicker);
a full refresh runs every 8 fast refreshes, and whenever more than 3% of the
pixels change at once, to clear ghosting. Both thresholds are `#define`s at the
top of the sketch.

If you see random speckling that a refresh fixes, try lowering `EPD_SPI_HZ` from
10 MHz to 2 MHz before touching anything else, then `USE_HW_SPI 0`, then
`MAX_RAPIDS 0`. Changing one at a time tells you which it was.

---

## Credits

The display driver — the initialisation registers, the GC and DU waveform tables,
and the power-up sequence including GPIO41 — is taken from Elecrow's own example
code for this board, in the `arduino_A_green_circular_sticker_on_the_back` folder
of their repository:

<https://github.com/Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300>

That repository carries no licence file, so its terms are whatever Elecrow
intends them to be. It is reproduced here because it is the only sequence that
drives the newer panel revision, and because anyone hitting the same
`Busy Timeout!` wall deserves to find the answer.
