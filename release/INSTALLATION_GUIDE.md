# PStream PlayStation 5 Installation Guide

## What is PStream on PS5?
PStream is an open-source media player for jailbroken PlayStation 5 consoles.

* **`pstream-install.elf`**: An ELF payload that registers a native PStream icon on your PS5 Dashboard / Home screen.
* **`server.py`**: A lightweight Python streaming server running on your PC or home network that proxies media streams without ads.
* **`www/`**: The web application running inside the PS5's hardware-accelerated WebKit engine.

---

## Method 1: Windows PC (PowerShell - Recommended)
1. Extract the release folder on your PC.
2. Ensure your PS5 is on the same Wi-Fi/Ethernet network.
3. On your PS5, run your jailbreak (etaHEN or elfldr active on port 9021).
4. On your PC, open PowerShell in the extracted folder and run:
   ```powershell
   .\send_elf.ps1 -Ps5Host 192.168.1.XX -Elf pstream-install.elf
   ```
   *(Replace `192.168.1.XX` with your PS5's IP address found in Settings > Network).*
5. The PStream icon will immediately appear on your PS5 Dashboard.
6. Run `python server.py` on your PC, then launch PStream from your PS5 dashboard!

---

## Method 2: PS5Upload / Netcat (PC / Mac / Linux / Phone)
1. Open your payload sender tool (e.g. **PS5Upload** or terminal).
2. Set Target IP to your PS5 IP and Port to `9021`.
3. Select and send `pstream-install.elf`.
   Command line alternative:
   ```bash
   nc -q0 192.168.1.XX 9021 < pstream-install.elf
   ```
4. PStream installs directly to your PS5 home screen.

---

## Method 3: USB Drive / File Manager (Itemzflow)
1. Format a USB drive as **exFAT** or **FAT32**.
2. Copy `pstream-install.elf` to the root of the USB drive.
3. Plug the USB into your PS5.
4. Launch **Itemzflow** or the **PS5 File Manager**.
5. Navigate to `/mnt/usb0/pstream-install.elf` and launch/execute it.

---

## DualSense Controller Controls
* **D-Pad / Left Stick**: Move selection
* **Cross (X)**: Select / Play / Pause
* **Circle (O)**: Back / Return to Home
* **Square**: Context Menu on cards / Clear search text in Search
* **Triangle**: Quick Search (opens dedicated Search tab)
* **L1 / R1**: Cycle tabs (Search, Home, Movies, TV, Anime, Continue Watching)
* **Options / Start**: Context menu (Add to favorites, Details)
* **Rewind / Fast Forward**: Jump 10 seconds back / forward

---

## Support the Developer
☕ Ko-fi: https://ko-fi.com/duckyiux1
