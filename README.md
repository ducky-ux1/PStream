# PStream - PlayStation 5 & 4 Native Media Player (v2.0)

<p align="center">
  <img src="logo.png" alt="PStream Logo" width="220" />
</p>

<p align="center">
  <strong>An open-source, 100% free streaming application designed natively for PlayStation 5 and PlayStation 4.</strong><br>
  100% Standalone console experience • Zero PC required • Hardware-accelerated HLS • DualSense wireless controller engine • Zero ads.
</p>

<p align="center">
  <a href="https://ko-fi.com/duckyiux1"><img src="https://img.shields.io/badge/Support%20on-Ko--fi-ff5e5b?style=for-the-badge&logo=ko-fi&logoColor=white" alt="Support on Ko-fi"></a>
  <img src="https://img.shields.io/badge/Platform-PS5%20%7C%20PS4-003791?style=for-the-badge&logo=playstation&logoColor=white" alt="PlayStation">
  <img src="https://img.shields.io/badge/License-GPL--3.0-green?style=for-the-badge" alt="License">
  <img src="https://img.shields.io/badge/Version-v2.0%20Standalone-red?style=for-the-badge" alt="Version">
</p>

---

## About The Project

**PStream** is a free, open-source streaming app created specifically for the PlayStation community. It delivers a modern, high-fidelity TV interface directly to your PS5 dashboard, powered by native hardware video decoding, zero advertisements, and seamless DualSense navigation.

* **100% Standalone (Zero PC Required):** After running the installer payload once on your PS5, no computer or external server is ever required. You can turn off your PC, pick up your DualSense controller, and stream movies and TV shows directly from your console.
* **100% Free & Always Free:** No subscriptions, no registration, no hidden paywalls, and **zero ads**.
* **Current Language Support:** English (`ENG`) and Italian (`ITA`) catalogs and streams are fully supported out-of-the-box, with Spanish, German, French, and Anime providers active.
* **DualSense Controller Engine:** Tailored specifically for PlayStation DualSense controllers with edge-triggered button mappings, analog deadzones, and silky-smooth D-pad navigation.
* **Smart Continue Watching:** Remembers your exact episode and timestamp (hour, minute, second) so you can resume your media with a single button press.

> [!NOTE]  
> If you enjoy PStream and want to support continued development, new providers, and upcoming PlayStation homebrew projects, you can support me on Ko-fi:  
> **[https://ko-fi.com/duckyiux1](https://ko-fi.com/duckyiux1)** — Thank you for your support!

---

## How PStream Works on PlayStation 5

PStream runs directly inside the PS5's high-performance hardware WebKit environment (Title ID: `PSTR00001`):

1. **Dashboard App Tile (`pstream-install.elf`):**
   * An ELF installer payload compiled with `ps5-payload-sdk`.
   * When executed once on your jailbroken PS5, it registers a media application icon (`icon0.png`) and splash screen (`pic1.png`) directly onto your **PS5 Home Screen / Dashboard**.
   * Launching this icon opens PStream in borderless fullscreen mode with full GPU acceleration.

2. **Standalone Client & Cloud Edge Architecture:**
   * **Client-Side TMDb Engine:** Browsing, search, billboards, categories, cast, seasons, and episodes are fetched directly by your PS5 with zero server overhead.
   * **24/7 Cloud Edge Stream Resolver:** Video streams are extracted and proxied by a 24/7 serverless edge proxy (or direct providers) so you never need a local PC running in the background.

---

## Installation Guide for PS5 Users

### Prerequisites
* A PlayStation 5 console running jailbreak firmware (**3.00 - 4.51**) with **etaHEN** or an **ELF Loader (`elfldr`)** active on port `9021`.

---

### Method 1: Send via PS5Upload (Recommended)

1. Download **`pstream-install-elf.zip`** from the [Releases](../../releases) tab and extract `pstream-install.elf`.
2. Open **PS5Upload** (or your payload sender tool on PC, Mac, Linux, or Android).
3. Set Target IP to your PS5's IP address and Port to `9021`.
4. Select `pstream-install.elf` and click **Send**.
5. Look at your TV screen — the **PStream** icon will immediately appear on your PS5 Dashboard!
6. **You're done!** Turn off your PC, grab your controller, and launch PStream directly from your PS5 dashboard.

---

### Method 2: USB Drive / File Manager (Itemzflow)

1. Format a USB drive as **exFAT** or **FAT32**.
2. Copy `pstream-install.elf` to the root of the USB drive.
3. Plug the USB drive into your PS5.
4. Launch **Itemzflow** or the **PS5 File Manager**.
5. Navigate to `/mnt/usb0/pstream-install.elf` and execute it.
6. PStream is now installed to your home screen.

---

### Method 3: Netcat / Terminal (Linux / macOS / Windows)

```bash
nc -q0 <YOUR_PS5_IP> 9021 < pstream-install.elf
```

---

### Method 4: Easy PowerShell (Windows)

1. Download and extract **`PStream-PS5-v2.0.zip`**.
2. Open PowerShell in the extracted directory and run:
   ```powershell
   .\send_elf.ps1 -Ps5Host <YOUR_PS5_IP> -Elf pstream-install.elf
   ```
3. PStream installs immediately to your PS5 dashboard.

---

## DualSense Controller Mapping

| Button | Browse & Navigation | Video Player View | Context Menu |
|---|---|---|---|
| **D-Pad / Left Stick** | Move selection between cards & shelves | Navigate player HUD controls | Navigate menu items |
| **Cross (✕)** | Select / Play title / View details | Toggle Play / Pause | Confirm action |
| **Circle (◯)** | Back / Return to Home | Exit player, return to browse | Close menu / modal |
| **Square (▢)** | Open card context menu / Clear search | Quick actions | Close menu |
| **Triangle (△)** | Jump directly to Search tab | - | - |
| **L1 (Left Shoulder)** | Switch tabs from right to left | Seek 30 seconds back | - |
| **R1 (Right Shoulder)** | Switch tabs from left to right | Seek 30 seconds forward | - |
| **Options / Start** | Open Card Context Menu (Favorites, Remove, Details) | Toggle Player HUD overlay | Close menu |
| **Rewind / Forward** | - | Seek 10 seconds back / forward | - |

---

## Project Structure

```
PStream/
├── pstream-install.elf     # Compiled PS5 dashboard installer payload
├── worker.js              # Standalone Cloudflare Worker stream resolver
├── index.html             # PS5 WebKit user interface
├── app.css                # Netflix-style dark theme & typography
├── app.js                 # Standalone TMDb engine & DualSense controller driver
├── input.js               # Hardware gamepad & keycode normalizer
├── hls.min.js             # Hardware-buffered HLS video playback pipeline
├── docs/                  # GitHub Pages distribution directory
├── ps5/
│   ├── installer/         # Source code & Makefile for pstream-install.elf
│   └── scripts/           # Deployment & test utilities
└── release/               # Production release zips for GitHub Releases
```

---

## License

This project is licensed under the **GNU General Public License v3.0** (GPL-3.0). See [LICENSE](LICENSE) for details.
