# Wisp

![platform](https://img.shields.io/badge/platform-ESP32--C6-blue)
![framework](https://img.shields.io/badge/framework-ESP--IDF%205.x-orange)
![license](https://img.shields.io/badge/license-MIT-green)

**Wisp is a tiny WiFi bridge for the ESP32-C6.** It joins a WiFi network you
already have, and re-shares that internet over its *own* WiFi network. Devices
like a Raspberry Pi and a laptop connect to Wisp instead of to your router, get
online, and can talk to each other on one small private network.

You set everything up from your phone or laptop through a web page — there are
no passwords baked into the code.

> [!IMPORTANT]
> Only use Wisp with a network you are **allowed** to share: your own phone
> hotspot, your home WiFi, or an approved guest network. Putting Wisp on a
> managed/company network you are not permitted to share from makes it a "rogue
> access point", which is against the rules of most such networks.

---

## What you need

**Hardware**

- An **ESP32-C6 Super Mini** board (other ESP32-C6 boards work too).
- A **USB-C cable** that can transfer data (not a charge-only cable).
- The WiFi network you want to share, plus its password.
- The devices you want to bring online (e.g. a Raspberry Pi and a laptop).

**Software (on your computer)**

- **ESP-IDF v5.3 or newer** — Espressif's official toolkit. Installing it is the
  one-time setup; see Step 1 below.

You do **not** need a separate USB driver (like CH340): the ESP32-C6 talks to
your computer over native USB.

---

## What the LED tells you

The little RGB LED on the board (GPIO8) is your status display:

| Color                | Meaning                                       |
| -------------------- | --------------------------------------------- |
| Blue (steady)        | Setup mode — the configuration page is open   |
| Orange (blinking)    | Connecting to your WiFi network               |
| Green (steady)       | Connected — internet is being shared          |
| Red (blinking)       | Lost the connection — retrying automatically  |

---

## Step 1 — Install ESP-IDF (one time)

ESP-IDF is the toolchain that turns this code into firmware. Pick the easiest
option for you:

- **Beginner-friendly (recommended):** install the **ESP-IDF extension for VS
  Code**. Open VS Code, go to Extensions, search "ESP-IDF", install it, then run
  the "Configure ESP-IDF Extension" wizard and choose version **5.3** (or newer).
  Guide: <https://github.com/espressif/vscode-esp-idf-extension>
- **Windows installer:** download the ESP-IDF Tools Installer from
  <https://dl.espressif.com/dl/esp-idf/> and follow the prompts.
- **macOS / Linux command line:** follow
  <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/>

After installing, open the **ESP-IDF terminal** (the VS Code extension provides
one, or run `export.sh` / `export.bat` from your IDF install). When
`idf.py --version` prints a version, you're ready.

---

## Step 2 — Get the code

```sh
git clone https://github.com/AchimPieters/wisp.git
cd wisp
```

(Or download the ZIP from the green **Code** button on GitHub and unzip it.)

---

## Step 3 — Build and flash

Plug the board into your computer with the USB-C cable, then run:

```sh
idf.py set-target esp32c6
idf.py build
idf.py -p <PORT> flash monitor
```

What each line does:

- `set-target esp32c6` — tells ESP-IDF you're building for the C6 (only needed
  once).
- `build` — compiles the firmware. The first build also downloads the
  `led_strip` component automatically.
- `flash monitor` — uploads the firmware and then shows the board's log output.

Replace `<PORT>` with your board's port:

- **Linux:** `/dev/ttyACM0`
- **macOS:** `/dev/cu.usbmodem…` (run `ls /dev/cu.*` to find it)
- **Windows:** `COM3`, `COM4`, … (check Device Manager, "Ports")

> Tip: in the VS Code extension you can skip the commands and use the bottom-bar
> buttons: select target, then Build, Flash, Monitor.

To leave the monitor, press **Ctrl+]**.

---

## Step 4 — First-time setup (the web page)

1. Power the board. The LED turns **blue** — it has started a setup network.
2. On your phone or laptop, open WiFi settings and connect to **`Wisp-Setup`**
   (password: **`wispsetup`**).
3. A configuration page should pop up automatically. If it doesn't, open a
   browser and go to **`http://192.168.4.1`**.
4. Under "Kies een WiFi-netwerk" the nearby networks appear. Tap the one you want
   to share from. (No network shown? Tap **Vernieuwen** to scan again, or **Ander
   netwerk kiezen** to type a hidden name.)
5. Type that network's **password** in the *Wachtwoord* field.
6. Under "Eigen netwerk (Access Point)" choose a **name** and **password**
   (min. 8 characters) for the new network Wisp will create.
7. Tap **Opslaan en verbinden**. The board saves and reboots.

After the reboot the LED goes **orange** (connecting) and then **green** when
it's online and sharing internet.

---

## Step 5 — Connect your devices

On your Raspberry Pi, laptop, etc., connect to the network name and password you
chose in step 6. They now have internet through Wisp, and they're all on the
`192.168.4.x` network, so they can reach each other directly (SSH, file
transfer, a web interface, and so on). Wisp itself is always at `192.168.4.1`.

---

## Changing the settings later

Open a browser on a device connected to Wisp and go to **`http://192.168.4.1`**
to see the status page and a button to reconfigure.

## Resetting (wipe the configuration)

- **From the web page:** go to `http://192.168.4.1` and click *Opnieuw
  instellen*.
- **With the button:** power the board on, then press and hold the **BOOT**
  button (GPIO9) within about 1.5 seconds. The configuration is erased and Wisp
  returns to setup mode (blue LED).
  *Note:* GPIO9 is a "strapping" pin, so don't hold it at the moment of
  power-on/reset — press it just after the board starts.

---

## Troubleshooting

| Problem | Fix |
| --- | --- |
| No port shows up when flashing | Use a data-capable USB-C cable; try another port; make sure the board is plugged in. The C6 needs no driver. |
| `idf.py: command not found` | Open the ESP-IDF terminal first (run `export.sh` / `export.bat`, or use the VS Code extension's terminal). |
| Build fails on `led_strip` | You need ESP-IDF **5.x**, and an internet connection on the first build (the component is downloaded then). |
| `Wisp-Setup` network doesn't appear | Wait a few seconds after power-on; the LED must be blue. If not, re-flash. |
| Connected to my AP but no internet | Make sure the LED is **green**. If names don't resolve it's usually DNS — reboot once; Wisp falls back to a public DNS if your network's DNS isn't known yet. |
| LED stays off or shows wrong colors | Your board may use a different LED pin. Change `LED_GPIO` in `main/status_led.c`. |
| Slow / video stutters | An ESP as a router has limited throughput. Great for management and light browsing, not for heavy video. |

---

## How it works

Wisp runs the WiFi radio in access-point + station mode at the same time. The
station side joins your upstream network; the access-point side serves your
devices. NAPT (the same NAT a home router does) routes traffic between the two.
On first boot — or after a reset — it instead opens a captive portal (a small
web server plus a DNS trick that makes the setup page pop up) so you can enter
your settings, which are saved in flash (NVS).

## Project structure

```
wisp/
├── CMakeLists.txt              # top-level project file
├── sdkconfig.defaults          # enables IP forwarding + NAPT, larger app partition
├── .github/workflows/build.yml # CI: compiles the firmware on every push
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml        # pulls in espressif/led_strip
    ├── main.c                  # init, mode select, WiFi AP+STA, NAPT, reset button
    ├── config_store.c/.h       # saves/loads settings in NVS
    ├── portal.c/.h             # captive portal: web server, DNS hijack, scan, save
    └── status_led.c/.h         # WS2812 status indicator
```

## Things you can tweak

- **LED pin:** `LED_GPIO` in `main/status_led.c` (default 8).
- **LED brightness/colors:** the `set_rgb(...)` values in `main/status_led.c`.
- **Setup network name/password:** the `set_ap_config("Wisp-Setup", "wispsetup")`
  line in `main/main.c`.

## Limitations

- Upstream uses WPA2-Personal (password only); WPA2-Enterprise
  (username + password) is not supported.
- All device-to-device traffic is relayed by the single radio; a network scan in
  setup mode can briefly stutter the portal connection.
- WS2812 LEDs are driven over RMT; under heavy WiFi load an occasional wrong
  color flash can happen. Harmless for a status light.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Achim Pieters.
