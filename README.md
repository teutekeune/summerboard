# Summerboard
![Summerboard Banner](summerboardbanner.jpeg)
Summerboard is an iOS-style tile springboard, gesture-based app launcher, and window manager for X11 environments. Written in lightweight C and optimized for embedded single-board computers (SBCs), it features a custom software rasterizer, spring-based physics, smooth transitions, card-based multitasking, and built-in system panels.

---

## Technical Features
![Summerboard Demo](summerboardipadmini1.gif)
* **Two Cortex A53 cores and 512 Megabytes of ram.** The demo you see is running natiely on a hacked ipad mini 1. see https://github.com/rzxvx/Project-Cascadia/tree/main
* **Built for ARM SBCs:** Optimized with double-buffered shared memory (MIT-SHM) rendering and custom assembly-friendly blending algorithms.
* **Spring Physics Engine:** Fluid, dynamic UI animations (scaling, workspace swiping, window transitions).
* **Card-Based Multitasking:** iOS-style switcher with gesture support (swipe up to minimize, hold-and-drag to close processes).
* **On-Screen Keyboard Integration:** Native support for `matchbox-keyboard`.
* **Built-in Systems:** Integrated Wi-Fi configuration utility supporting real-time network scanning via `iw` and `wpa_supplicant`.
* **Zero Heavy Dependencies:** Requires only standard X11 runtime libraries (`libX11`, `libXext`).

---

## Hardware & Cross-Compilation Guide

To build `summerboard` for `armhf` (ARMv6/v7) devices (such as Raspberry Pi, Cascadia-based rootfs, or Alpine Linux boards) from an `x86_64` host machine, use Docker with QEMU user emulation.

### Requirements
* Docker installed on host machine
* `qemu-user-static` enabled for multi-arch builds

### Step-by-Step Build Steps

1. **Enable QEMU Emulation (Host):**
   ```bash
   docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
   ```

2. **Start the ARM32 Alpine Container:**
   ```bash
   docker run -it --rm --platform linux/arm/v6 -v $(pwd):/work -w /work arm32v6/alpine:latest sh
   ```

3. **Install Build Dependencies (Inside Container):**
   ```bash
   apk add --no-cache build-base xorg-server-dev libx11-dev
   ```

4. **Compile `summerboard`:**
   ```bash
   gcc -O3 -fno-math-errno -o summerboard summerboard.c -lX11 -lXext -lm -march=native
   ```

5. **Verify the Output Architecture:**
   ```bash
   file summerboard
   # Should output: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV)...
   ```

6. **Exit Container:**
   ```bash
   exit
   ```

---

## Native Compilation (Directly on Target Device)

If compiling directly on an ARM Linux target machine (Debian/Ubuntu/Alpine):

### Alpine Linux
```bash
sudo apk add build-base xorg-server-dev libx11-dev
gcc -O3 -fno-math-errno -o summerboard summerboard.c -lX11 -lXext -lm
```

### Ubuntu / Debian
```bash
sudo apt-get update
sudo apt-get install -y build-essential libx11-dev libxext-dev
gcc -O3 -fno-math-errno -o summerboard summerboard.c -lX11 -lXext -lm
```

---

## Usage & Execution

Run `summerboard` inside an active X11 display session:

```bash
./summerboard
```

### Key Bindings & Gestures
* **App Grid Navigation:** Click/drag left or right to switch page grids.
* **Open App:** Click any app icon.
* **App Switcher / Recent Apps:** Drag up from the bottom edge of the screen.
* **Close App:** Swipe up on an app card inside the task switcher view.
* **Toggle Virtual Keyboard:** Tap the `Keyboard` app icon to toggle `matchbox-keyboard`.

---

## Optional Runtime Dependencies

To utilize the full built-in utility set, ensure the following utilities are available on the target image:

* `matchbox-keyboard` *(Virtual Keyboard support)*
* `iw`, `wpa_supplicant`, `ip` *(Wi-Fi Control Panel)*
* `xterm` *(Default fallback terminal)*

---

## License

GPL V3 License. See `LICENSE` for details.
