# Antigravity OS History

Append-only log of what was done and why, **newest entry first**. Older entries move to
`docs/history-archive/<year>.md` when this file passes ~1500 lines.

### Re-enable Paratrooper, BLE debug flag, docs (feature/arm-ble-xbox, 2026-10-28)
- What: Paratrooper launches again on the Pi; Bluetooth debug rows and runtime [BLE] messages sit behind `BLE_DEBUG_SCREEN` (default 0, serial only). Docs updated for the verified Pi state.
- Why: pairing and input work on hardware; debug output drew over the game.
- Touched: `kmain.c`, `bcm43438.c`, `ble_host.c`, `bluetooth.h`, docs. Existing feature affected: the Pi boot screen (debug rows hidden by default).
- Verified: aarch64 and x86 build; not yet re-run on the Pi with the game.
- Open: none.

### Xbox controller pairing mode works (feature/arm-ble-xbox, 2026-10-28)
- What: before subscribing, the host waits for bonding, sends its identity keys (IRK + address), reads every readable HID value in full (Report Map via Read Blob), sends HID Control Point Exit Suspend, then writes every CCCD.
- Why: in pairing mode the controller kept flashing and sent no input while a normal power-on worked; it only finishes pairing for a host that behaves like a full HID host.
- Touched: `ble_host.c`.
- Verified: Pi 3B with Xbox Wireless Controller: pairing mode (light goes solid), normal power-on, Pi reboot; PC simulation of the GATT and bonding flows.
- Open: none.

### BLE host: pairing, GATT, input reports (feature/arm-ble-xbox, 2026-10-28)
- What: new `ble_host.c` (L2CAP, SMP legacy Just Works with software AES-128, encryption, GATT discovery, CCCD subscription, report routing); fixed Xbox button bits; on-screen debug rows.
- Why: the controller connected but sent nothing; pairing and GATT were missing.
- Touched: `ble_host.c`, `bcm43438.c`, `xbox_ble.c`, `bluetooth.h`.
- Verified: AES/c1/s1 against FIPS-197 and Core spec test vectors on the PC; buttons, sticks and triggers on hardware.
- Open: subscribing to every CCCD is a workaround because most value types arrive as unrecognised 128-bit UUIDs.

### HDMI audio driver (feature/arm-ble-xbox, 2026-10-28)
- What: `sound_vchiq.c` replaced by `sound_hdmi.c`: HDMI MAI FIFO fed by DMA from a looping 48 kHz IEC958 buffer, N/CTS, Audio InfoFrame.
- Why: the VCHIQ driver was a placeholder that never sent audio.
- Touched: `sound_hdmi.c`, `sound.h`, `Makefile`. Existing feature affected: Pi audio (now actually audible).
- Verified: Paratrooper sounds on HDMI TV speakers.
- Open: tone buffer loops every 100 ms, so non-matching tone periods have a small phase step.

### BCM43438 bring-up and LE connect (feature/arm-ble-xbox, 2026-10-27)
- What: radio power and clocks before UART, `-mstrict-align`, EL1 vectors with fault report, IRQ-driven UART RX ring, duplicate-filtered scan, controller match by HID UUID / gamepad appearance, LE connection (with Gemini's mailbox/HUD work).
- Why: chip silent (power/clock order), silent hang (unaligned store with MMU off), lost advertisements (16-byte FIFO).
- Touched: `boot.S`, `kmain.c`, `bcm43438.c`, `serial.c`, `Makefile`. Existing feature affected: console moved from PL011 to the mini-UART.
- Verified: Pi 3B connects to the controller.
- Open: none.
