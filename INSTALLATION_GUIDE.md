# PStream Installation & Troubleshooting Guide (v1.1.0)

Choose your console below. All release binaries are available on the [**GitHub Releases**](https://github.com/ducky-ux1/PStream/releases/latest) page.

---

## 1. PlayStation 4 Installation (`.pkg`)

**Supported Firmwares:** All jailbroken PS4 firmwares running GoldHEN or Mira (`5.05`, `6.72`, `7.02`, `7.55`, `9.00`, `11.00`, `12.00+`).

1. Download **`PStream-PS4-v1.1.0.pkg`** from [Releases](https://github.com/ducky-ux1/PStream/releases/latest).
2. Copy `PStream-PS4-v1.1.0.pkg` to the root directory of an **exFAT** or **FAT32** USB flash drive (or send it via Remote Package Installer / FTP).
3. Plug the USB drive into your PS4 and enable **GoldHEN**.
4. Go to **GoldHEN -> Debug Settings -> Package Installer** and install `PStream-PS4-v1.1.0.pkg`.
5. Launch **PStream** directly from your PS4 Home Screen.
   - To exit PStream on PS4, either hold the **PS Button -> Close Application**, or select **Exit App** inside **Settings**.
   - To uninstall on PS4, press **Options** on the PStream tile on your PS4 Home Screen and choose **Delete**.

---

## 2. PlayStation 5 Installation (`.elf`)

PStream on PS5 consists of two components embedded inside a single installer payload:
1. A native dashboard tile (`PSTR00001`) registered on the PS5 **Media** tab.
2. A lightweight background stream daemon (`pstream-service.elf`) configured to autostart on port `8086` whenever you run your jailbreak.

> **Upgrading from v1.0.x:** Simply run the new v1.1.0 `PStream-PayloadManager.elf` or `PStream-PS5Upload.elf` once. It automatically stops the older daemon, updates `param.json` (removing the legacy TinyURL redirect), preserves your existing `autoload.txt` entries, and starts v1.1.0 immediately.

### Option A: USB + Payload Manager (Recommended)

1. Download **`PStream-PS5-PayloadManager-v1.1.0.zip`** (or **`PStream-PayloadManager.elf`** directly) from [Releases](https://github.com/ducky-ux1/PStream/releases/latest).
2. Copy `PStream-PayloadManager.elf` to the root of a USB drive (or to `/data/pldmgr/payloads/` via FTP).
3. Open **Payload Manager** on your PS5 and execute **`PStream-PayloadManager.elf`**.
4. Switch to the **Media** tab at the top of your PS5 Home Screen and launch **PStream**.

### Option B: Network Injection (Android / Windows / Linux / macOS)

Make sure `elfldr` is listening on your PS5 (standard port **`9021`**).

- **Android (`PS5 Upload` app):**
  1. Download `PStream-PayloadManager.elf` (or extract `PStream-PS5Upload.elf` from `PStream-PS5-Upload-v1.1.0.zip`).
  2. Enter your PS5 IP address and port `9021`, then send the `.elf`.
- **Windows PowerShell:**
  ```powershell
  .\send_elf.ps1 -Ip 192.168.1.XX -Port 9021 -Elf .\PStream-PS5Upload.elf
  ```
- **Linux / macOS:**
  ```bash
  nc -w 3 192.168.1.XX 9021 < PStream-PS5Upload.elf
  ```

---

## 3. Uninstalling PStream on PlayStation 5

Because PS5 Media tab homebrew apps (`applicationCategoryType: 66048`) do not always expose a Delete button in the PS5 system shell, PStream provides a dedicated one-shot uninstaller payload:

1. Download **`PStream-Uninstall.elf`** (or **`PStream-PS5-Uninstaller-v1.1.0.zip`**) from [Releases](https://github.com/ducky-ux1/PStream/releases/latest).
2. Run `PStream-Uninstall.elf` via **Payload Manager** or send it to port **`9021`** via PS5 Upload / `nc`.
3. It will automatically:
   - Shut down the running `pstream-service.elf` background process on port `8086`.
   - Unregister `PSTR00001` via `sceAppInstUtilAppUnInstall`.
   - Delete `/system_ex/app/PSTR00001`, `/user/app/PSTR00001`, and `/user/appmeta/PSTR00001`.
   - Remove `pstream-service.elf` from `/data/pldmgr/autoload.txt` and `/data/ps5_autoloader/autoload.txt` while keeping all of your other payloads untouched.

---

## 4. Troubleshooting

### PS5 shows "Waiting for Local Engine" on launch
- The PS5 dashboard tile connects to `127.0.0.1:8086`. After cold-booting your PS5 and running your jailbreak, make sure your payload autoloader (`Payload Manager`, `ps5_autoloader`, or `etaHEN`) has finished starting payloads — the waiting screen automatically launches PStream the moment port `8086` responds.
- If you do not use an autoloader, run `PStream-PayloadManager.elf` once from Payload Manager to start the background service.

### PS5 uses a Jailbreak DNS Blocker
- Ensure `ducky-ux1.github.io`, `raw.githubusercontent.com`, `api.themoviedb.org`, and `image.tmdb.org` are not blocked by your router or custom DNS rules. (Most standard PlayStation update-blocking DNS servers allow GitHub Pages and TMDb by default.)

### Changing Audio / Catalog Language
- Open **PStream -> Settings -> Preferred Language / Provider** and choose between **English (`ENG`)**, **Deutsch (`GER`)**, **Italiano (`ITA`)**, **Español (`ESP`)**, **Français (`FRA`)**, or **Português (`POR`)**. If a localized audio track is not available for a specific title, PStream automatically falls back to English so playback still succeeds.
