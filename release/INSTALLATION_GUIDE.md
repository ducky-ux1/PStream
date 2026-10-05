# PStream PlayStation 5 - Standalone Installation Guide (v2.0)

**100% STANDALONE CONSOLE MEDIA PLAYER • ZERO PC REQUIRED**

---

## What is PStream on PS5?
PStream is a free, open-source media player designed natively for jailbroken PlayStation 5 consoles.

* **100% Standalone:** No PC or server is needed after installation. The app connects directly to cloud media backends from your PS5.
* **Native Dashboard Tile:** Installs a clean PStream icon directly into your PS5 Dashboard / Media screen.
* **DualSense Spatial Navigation:** Full D-Pad, Left Stick, L1/R1 tab cycling, and Options menu support.
* **Zero Ads Pipeline:** Ad-free HLS streaming engine with multi-language provider support.

---

## Method 1: PS5Upload (Recommended for Everyone)
1. Download **`pstream-install-elf.zip`** from the Releases tab and extract `pstream-install.elf`.
2. On your PS5, run your jailbreak (etaHEN or ELF loader active on port **9021**).
3. Open **PS5Upload** (or your preferred payload sender app on PC, Mac, Linux, or Android).
4. Enter your PS5 IP address and set the port to **9021**.
5. Select `pstream-install.elf` and click **Send Payload**.
6. The PStream tile will immediately appear on your PS5 Dashboard.
7. **You are done!** Turn off your PC, grab your DualSense controller, and launch PStream directly from your PS5!

---

## Method 2: USB Drive / Itemzflow File Manager (No Network Sender Needed)
1. Format a USB drive as **exFAT** or **FAT32**.
2. Copy `pstream-install.elf` onto the USB drive.
3. Plug the USB into your PS5.
4. Launch **Itemzflow** or the **PS5 File Manager**.
5. Navigate to `/mnt/usb0/pstream-install.elf` and execute it.
6. The PStream tile is installed on your dashboard. Unplug your USB and enjoy!

---

## Method 3: Netcat / Terminal (Linux / Mac / Windows)
Run this command from your terminal:
```bash
nc -q0 192.168.1.XX 9021 < pstream-install.elf
```
*(Replace `192.168.1.XX` with your PS5's IP address found in Settings > Network > Connection Status).*

---

## Method 4: Windows PowerShell (Single Command)
1. Open PowerShell in the release folder.
2. Run:
```powershell
.\send_elf.ps1 -Ps5Host 192.168.1.XX -Elf pstream-install.elf
```
3. PStream installs instantly to your PS5 dashboard.

---

## DualSense Controller Controls

| Button | Action in Browse View | Action in Video Player View |
|---|---|---|
| **D-Pad / Left Stick** | Move selection between cards & shelves | Navigate player HUD controls |
| **Cross (✕)** | Select / Open Title Details | Toggle Play / Pause |
| **Circle (◯)** | Back / Return to Home | Exit player, return to browse |
| **Square (▢)** | Open card context menu / Clear search | Quick actions |
| **Triangle (△)** | Jump to dedicated Search tab | - |
| **L1 / R1** | Switch tabs (Search, Home, Movies, TV, Anime, Continue) | Jump 30 seconds back / forward |
| **Rewind / Forward** | - | Jump 10 seconds back / forward |
| **Options Button** | Open Context Menu (Favorites / Details / Remove) | Toggle Player HUD overlay |

---

## Support the Developer
PStream is completely free and open-source. If you enjoy using it, you can support future development and more language providers here:  
☕ **Ko-fi:** https://ko-fi.com/duckyiux1
