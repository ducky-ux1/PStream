# PStream - PlayStation 5 & 4 Native Media Player

<p align="center">
  <img src="ps5/www/logo.png" alt="PStream Logo" width="220" />
</p>

<p align="center">
  <strong>An open-source, 100% free streaming and media-playing application designed specifically for PlayStation 5 and PlayStation 4.</strong><br>
  Built with native WebKit hardware acceleration, zero ads, and full DualSense wireless controller support.
</p>

<p align="center">
  <a href="https://ko-fi.com/duckyiux1"><img src="https://img.shields.io/badge/Support%20on-Ko--fi-ff5e5b?style=for-the-badge&logo=ko-fi&logoColor=white" alt="Support on Ko-fi"></a>
  <img src="https://img.shields.io/badge/Platform-PS5%20%7C%20PS4-003791?style=for-the-badge&logo=playstation&logoColor=white" alt="PlayStation">
  <img src="https://img.shields.io/badge/License-GPL--3.0-green?style=for-the-badge" alt="License">
  <img src="https://img.shields.io/badge/Version-v1.3-red?style=for-the-badge" alt="Version">
</p>

---

## About The Project

**PStream** is a free and open-source streaming app created for the PlayStation community. It delivers a modern, Netflix-style TV interface directly to your PS5 dashboard, powered by native hardware video decoding, ad-stripping proxies, and seamless DualSense navigation.

* **100% Free & Always Free:** No subscriptions, no premium tiers, no hidden paywalls, and **zero ads**.
* **Current Language Support:** English (`ENG`) and Italian (`ITA`) are fully supported. Additional languages and community providers (German, Spanish, French, Anime catalogs) are actively being developed for future releases.
* **DualSense Controller Engine:** Tailored specifically for PlayStation DualSense controllers with edge-triggered inputs, analog deadzones, and hold-to-repeat D-pad navigation.
* **Smart Continue Watching:** Remembers your exact episode and timestamp (hour, minute, second) so you can resume your movies and shows with one press.

> [!NOTE]  
> If you enjoy PStream and want to support continued development, new providers, and upcoming PlayStation homebrew projects, you can support me on Ko-fi:  
> **[https://ko-fi.com/duckyiux1](https://ko-fi.com/duckyiux1)** — Thank you for your support!

---

## How PStream Works on PlayStation 5

PStream on PS5 consists of two components:

1. **Dashboard App Tile (`pstream-install.elf`):**
   * An ELF payload compiled with `ps5-payload-sdk` (Title ID: `PSTR00001`).
   * When executed once on your jailbroken PS5, it registers a permanent media application icon (`icon0.png`) and splash screen (`pic1.png`) directly onto your **PS5 Home Screen / Dashboard**.
   * Launching this icon opens PS5 WebKit in dedicated fullscreen app mode (no browser address bar, full hardware decoding enabled).

2. **Streaming Backend (`server.py`):**
   * A lightweight Python server running locally on your PC, Raspberry Pi, home server, or console.
   * It handles metadata scraping (TMDB), provider resolution, HLS video stream proxying, and removes all web ads before video data reaches your console.

---

## Installation Guide for PS5 Users

### Prerequisites
* A PlayStation 5 console running firmware **3.00 - 4.51** (or compatible jailbreak firmware) with **etaHEN** or an **ELF Loader (`elfldr`)** active on port `9021`.
* Your PS5 and PC/device connected to the same local Wi-Fi or LAN network.

---

### Method 1: Easy Install via PowerShell (Windows PC)

1. Download the latest **`PStream-PS5-v1.3.zip`** from the [Releases](../../releases) tab and extract it.
2. Note your PS5's IP address (found in PS5 *Settings > Network > Connection Status*).
3. Open PowerShell inside the extracted folder and run:
   ```powershell
   .\send_elf.ps1 -Ps5Host <YOUR_PS5_IP> -Elf pstream-install.elf
   ```
4. Look at your TV screen — the **PStream** icon will appear on your PS5 Dashboard!

---

### Method 2: Send via PS5Upload / Netcat (PC, Mac, Linux, Phone)

If you use tools like **PS5Upload**, Netcat, or mobile payload senders:

1. Launch your PS5 exploit so `elfldr` is listening on port `9021`.
2. Send `pstream-install.elf` using your preferred payload sender or command line:
   ```bash
   nc -q0 <YOUR_PS5_IP> 9021 < pstream-install.elf
   ```
3. The installer remounts `/system_ex` and installs Title ID `PSTR00001` onto your home screen.

---

### Method 3: USB Drive / File Manager (Itemzflow)

1. Format a USB drive as **exFAT** or **FAT32**.
2. Copy `pstream-install.elf` to the root of your USB drive.
3. Plug the USB drive into your PS5.
4. Open **Itemzflow** or the **PS5 File Manager** homebrew app.
5. Navigate to `/mnt/usb0/pstream-install.elf` and launch/execute it.

---

### Running the Streaming Backend

On your PC or home server (in the folder containing `server.py` and `www/`):
```bash
python server.py
```
*The server will start listening on port `8080`. Your PS5 PStream app will automatically connect to it over your local network.*

---

## DualSense Controller Navigation

| Button | Action |
| :--- | :--- |
| **D-Pad / Left Stick** | Navigate shelves, cards, menus, and on-screen keyboard |
| **Cross (X)** | Select / Play / Pause / Activate focused item |
| **Circle (O)** | Back / Return to Home / Dismiss menus & dialogs |
| **Square** | Open Card Context Menu on Home / Clear search text in Search |
| **Triangle** | Quick Search (opens dedicated Search tab immediately) |
| **L1** | Cycle tabs from right to left |
| **R1** | Cycle tabs from left to right |
| **Options / Start** | Card Options Menu (Add to Watchlist, Mark Watched, Details) |
| **Rewind 10s (`<<`)** | Jump back 10 seconds (configurable to 15s or 30s) |
| **Forward 10s (`>>`)** | Jump forward 10 seconds (configurable to 15s or 30s) |

---

## Features

- [x] **Dedicated Search Tab:** Live search with instant thumbnail grid right at the top of the screen as you type.
- [x] **Smart Continue Watching:** Resume exactly where you left off (tracks Season, Episode, hours, minutes, seconds).
- [x] **Clean Video Player:** Modern OSD overlay with seek buttons, audio/subtitle selector, episode browser, and aspect ratio controls.
- [x] **Silky-Smooth 60 FPS Viewport:** Smooth cubic-ease vertical scrolling optimized for TV screens.
- [x] **Zero Ads Guarantee:** All streams are scrubbed of ads, popups, and redirects at the proxy level.
- [x] **DualSense WebKit Input Engine:** Native handling of PS5 hardware keycodes (`116` L1, `117` R1, `114` Options, `113` Square, `112` Triangle).
- [x] **Customizable Settings:** Adjust skip jump intervals (10s, 15s, 30s), subtitle sizes (normal, large, extra large), autoplay, and provider language.

---

## Building from Source

### Prerequisites
* Linux or WSL2 (Ubuntu 22.04 recommended)
* `ps5-payload-sdk` installed and configured in your environment

### Build Steps
```bash
git clone https://github.com/duckyiux1/PStream.git
cd PStream/ps5/installer
make
```
This produces `pstream-install.elf` with embedded `icon0.png`, `pic1.png`, and `param.json`.

---

## Support & Donations

PStream is and will always remain completely free and open-source. If you appreciate the work and want to support server costs or future PlayStation homebrew projects:

☕ **Support on Ko-fi:** [https://ko-fi.com/duckyiux1](https://ko-fi.com/duckyiux1)

---

## License

This project is licensed under the GNU General Public License v3.0 - see the LICENSE file for details.
