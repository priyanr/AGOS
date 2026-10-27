#!/usr/bin/env bash
# Antigravity OS Multi-Architecture Build Script
# Usage: ./build.sh [x86_64 | aarch64 | all]

set -e

ARCH_ARG="${1:-x86_64}"

# Normalize architecture alias
case "$ARCH_ARG" in
    x86_64|amd64|x86)
        TARGET_ARCH="x86_64"
        ;;
    aarch64|arm64|rpi|raspberrypi)
        TARGET_ARCH="aarch64"
        ;;
    all)
        TARGET_ARCH="all"
        ;;
    clean)
        echo "Cleaning build artifacts..."
        make clean
        exit 0
        ;;
    *)
        echo "Usage: $0 [x86_64 | aarch64 | all | clean]"
        echo "  x86_64  : Builds hybrid ISO and ext4 disk image for PC (Limine)"
        echo "  aarch64 : Builds kernel8.img and bootable SD image for Raspberry Pi 3/4"
        echo "  all     : Builds both architectures"
        exit 1
        ;;
esac

# Function to check and install missing packages on Debian/Ubuntu/WSL
install_dependencies() {
    local arch="$1"
    local missing_pkgs=()

    # Common utilities
    command -v make >/dev/null 2>&1 || missing_pkgs+=("build-essential")
    command -v parted >/dev/null 2>&1 || missing_pkgs+=("parted")
    command -v mcopy >/dev/null 2>&1 || missing_pkgs+=("mtools")

    if [ "$arch" = "x86_64" ] || [ "$arch" = "all" ]; then
        command -v gcc >/dev/null 2>&1 || missing_pkgs+=("gcc")
        command -v xorriso >/dev/null 2>&1 || missing_pkgs+=("xorriso")
        command -v mkfs.ext4 >/dev/null 2>&1 || missing_pkgs+=("e2fsprogs")
    fi

    if [ "$arch" = "aarch64" ] || [ "$arch" = "all" ]; then
        command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || missing_pkgs+=("gcc-aarch64-linux-gnu")
        command -v aarch64-linux-gnu-ld >/dev/null 2>&1 || missing_pkgs+=("binutils-aarch64-linux-gnu")
        command -v mkfs.vfat >/dev/null 2>&1 || missing_pkgs+=("dosfstools")
    fi

    if [ ${#missing_pkgs[@]} -gt 0 ]; then
        echo "=========================================================="
        echo " [DEP] Missing build tools detected: ${missing_pkgs[*]}"
        echo " [DEP] Attempting automatic installation via apt..."
        echo "=========================================================="
        if command -v apt-get >/dev/null 2>&1; then
            if [ "$EUID" -eq 0 ]; then
                apt-get update && apt-get install -y "${missing_pkgs[@]}"
            elif command -v sudo >/dev/null 2>&1; then
                sudo apt-get update && sudo apt-get install -y "${missing_pkgs[@]}"
            else
                echo " [ERROR] Root/sudo required to install: ${missing_pkgs[*]}"
                exit 1
            fi
        else
            echo " [WARN] Non-apt system detected. Please install: ${missing_pkgs[*]}"
        fi
    fi
}

build_architecture() {
    local arch="$1"
    echo ""
    echo "=========================================================="
    echo " Building Antigravity OS for architecture: ${arch}"
    echo " Output directory: output/${arch}"
    echo "=========================================================="

    make ARCH="${arch}" all

    echo ""
    echo " [SUCCESS] Build completed for ${arch}."
    echo " Artifacts generated in output/${arch}:"
    ls -lh "output/${arch}"
    echo "=========================================================="
}

# Run dependency check
install_dependencies "$TARGET_ARCH"

# Build requested target
if [ "$TARGET_ARCH" = "all" ]; then
    build_architecture "x86_64"
    build_architecture "aarch64"
else
    build_architecture "$TARGET_ARCH"
fi

echo ""
echo "Quick Run Instructions:"
if [ "$TARGET_ARCH" = "x86_64" ] || [ "$TARGET_ARCH" = "all" ]; then
    echo "  PC / x86_64 (QEMU):"
    echo "    make ARCH=x86_64 run"
fi
if [ "$TARGET_ARCH" = "aarch64" ] || [ "$TARGET_ARCH" = "all" ]; then
    echo "  Raspberry Pi (QEMU):"
    echo "    make ARCH=aarch64 run      (Direct kernel8.img)"
    echo "    make ARCH=aarch64 run-sd   (Full SD card image)"
fi
echo ""
