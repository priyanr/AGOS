# Antigravity OS History

Append-only log of what was done and why, **newest entry first**. Older entries move to
`docs/history-archive/<year>.md` when this file passes ~1500 lines.
### Shared kernel verified on the Pi, demo video, code share (feature/arm-ble-xbox, 2026-10-28)
- What: no code change. Pi hardware check of the shared-kernel entry below; README gets the Pi Paratrooper + Xbox demo video (YouTube + `docs/rpi_paratrooper_xbox.mp4`) and an updated `git blame` code share (71% Claude Opus 5.5, 13% Gemini in this port, 16% earlier Gemini).
- Why: user confirmed everything works on the Pi; user request for the video and the share.
- Touched: `README.md`, `docs/status.md`, `docs/rpi_paratrooper_xbox.mp4`.
- Verified: on Pi 3B by the user: shell, SD card + FAT32 root, USB keyboard, Xbox controller and HDMI audio. One report of the controller needing pairing mode at boot turned out to be a test mistake.
- Open: none.

### One kernel for both CPUs: shell and utilities on the Pi, USB keyboard, SD card (feature/arm-ble-xbox, 2026-10-28)
- What: everything outside `kernel/arch/<arch>/` is now shared (shell, utilities, GW-BASIC, scheduler, PMM/heap, VFS, ext4, new read-only FAT32, partitions); x86 PC drivers moved to `kernel/arch/x86_64/`, Pi drivers to `kernel/arch/aarch64/drivers/`. New HAL (`hal/platform.h`) and shared `kernel_start()`. Pi: generic-timer tick and IRQ dispatch in C, context switch, SD card driver, DWC2 USB host with hub and HID keyboard.
- Why: user request: only HAL and drivers may differ per architecture; Pi defaults to the shell unless a config file on the filesystem says otherwise.
- Touched: Makefile (shared source list, `-mgeneral-regs-only` on ARM, `antigravity.cfg` in the SD image), sched, pmm, vmm.h, shell/gwbasic/paratrooper input loops. Existing features affected: Pi boots to the shell instead of the game loop (`LAUNCH_PARATROOPER` removed; `autostart=paratrooper` restores the game at boot, quitting opens the shell); x86 shell no longer freezes after `threads` (scheduler fix); x86 boot log says "Root filesystem mounted" instead of "ext4 root filesystem mounted".
- Verified: both architectures build with 0 warnings; x86 in QEMU (ext4 root, ls, threads, uname); Pi in QEMU raspi3 (SD, FAT32 ls/cat, config autostart, threads, GW-BASIC, fdisk) with audio init skipped (QEMU DMA hang, pre-existing). Not verified on hardware; USB not testable in QEMU 4.2.
- Open: Pi hardware test of SD card, USB keyboard, Bluetooth and audio with the new IRQ path.


### Firmware licensing: download at build time, Pi deployment guide (feature/arm-ble-xbox, 2026-10-28)
- What: Raspberry Pi boot firmware and the BCM43430A1 patchram are no longer committed; the Makefile downloads them from pinned upstream commits with their licence files, links the patchram unchanged via objcopy (no C array), and ships no device trees. Branch history rewritten to remove the binaries before the first push. README: Raspberry Pi deployment guide and third-party firmware section.
- Why: Broadcom (LICENCE.broadcom) and Cypress (LICENSE.cypress) terms require their notices and forbid modifying/translating the firmware; device trees are GPL and unused by the kernel.
- Touched: `Makefile`, `build.sh`, `.gitignore`, `bcm43438.c`, removed `bcm43430a1_hcd.c` and `firmware-rpi/*`, README, roadmap. Existing feature affected: SD card no longer contains .dtb files.
- Verified: clean aarch64 build downloads all files; patchram identical to the tested one; SD image content checked.
- Open: boot once from a freshly built card to confirm the Pi starts without device trees.

### Pi verified end to end (feature/arm-ble-xbox, 2026-10-28)
- What: no code change; hardware check of the three previous entries.
- Why: record that the MMU/cache speed-up, reliable HDMI audio start and the quit-to-title fix work on the Pi 3B.
- Touched: `docs/status.md`.
- Verified: full Paratrooper games with the Xbox controller at normal speed, sound after repeated power-unplug reboots, View / Xbox button returns to the title screen.
- Open: none.

### Blank screen on the Pi was Paratrooper quitting (feature/arm-ble-xbox, 2026-10-28)
- What: on the Pi, `kmain` restarts Paratrooper at the title screen whenever it exits.
- Why: View / Xbox (Guide) button (or ESC / q on serial) quits the game; on x86 that returns to the shell, on the Pi it left a black screen (the "Exited Paratrooper." line sits in the overscanned top row) while Bluetooth kept running. Pressing the Xbox button to wake the controller triggers it.
- Touched: `kernel/arch/aarch64/kmain.c`. Existing feature affected: quitting the game on the Pi now returns to the title screen.
- Verified: aarch64 build. Not yet run on the Pi.
- Open: whether the Xbox / View buttons should quit at all on the Pi (asked).

### HDMI audio silent on some boots after the speed-up (feature/arm-ble-xbox, 2026-10-28)
- What: real 2 ms timer waits after the MAI and DMA resets, memory barriers before starting the DMA and after rewriting the tone buffer, DMA start verified and retried once, and the DMA restarted if a tone is played while it is stopped.
- Why: after the MMU/cache change, sound worked on one boot and not on later ones (power unplugged). The old delay-loop wait after DMA reset shrank to about a microsecond, so the start could be ignored.
- Touched: `sound_hdmi.c`. [AUDIO] debug line shows rst (DMA restarts).
- Verified: aarch64 build. Not yet run on the Pi.
- Open: if still silent, one boot with BLE_DEBUG_SCREEN 1 and the [AUDIO] line.

### Pi performance: MMU + caches, max ARM clock (feature/arm-ble-xbox, 2026-10-28)
- What: identity-mapped MMU with data and instruction caches (`mmu.c`): RAM write-back, GPU memory/framebuffer and a GPU_SHARED block (mailbox and DMA buffers) uncached, peripherals Device. ARM clock raised to its maximum via mailbox. Mailbox and HDMI packet waits now use the ms timer.
- Why: Paratrooper was slow on the Pi; with the MMU off every access (each framebuffer pixel of the 1024x768 upscale) was an uncached strongly-ordered write.
- Touched: `mmu.c` (new), `linker.ld`, `arm.h`, `mailbox.c`, `kmain.c`, `sound_hdmi.c`, `bcm43438.c`, `Makefile`. Existing feature affected: all Pi code now runs with caches on.
- Verified: aarch64 build; QEMU raspi3 boots through MMU, mailbox and framebuffer (QEMU then stalls in the looping audio DMA, which it cannot emulate). Not yet run on the Pi.
- Open: confirm game speed, Bluetooth and audio on hardware.

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
