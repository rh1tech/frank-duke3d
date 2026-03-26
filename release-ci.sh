#!/bin/bash
#
# release-ci.sh - Non-interactive release build for CI/CD
#
# Usage: ./release-ci.sh <major> <minor>
#   e.g. ./release-ci.sh 1 04
#
# Same as release.sh but takes version as arguments instead of interactive input.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Version from arguments
if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <major> <minor>"
    echo "  e.g. $0 1 04"
    exit 1
fi

MAJOR=$((10#$1))
MINOR=$((10#$2))

# Validate
if [[ $MAJOR -lt 1 ]]; then
    echo "Error: Major version must be >= 1"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo "Error: Minor version must be 0-99"
    exit 1
fi

# Format version string
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo "Building release version: ${MAJOR}.$(printf '%02d' $MINOR)"

# Save new version
echo "$MAJOR $MINOR" > version.txt

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

# Build configurations: "BOARD CPU_SPEED PSRAM_SPEED MOS2 DESCRIPTION"
CONFIGS=(
    "M1 378 133 OFF medium-oc"
    "M1 504 166 OFF max-oc"
    "M2 378 133 OFF medium-oc"
    "M2 504 166 OFF max-oc"
)

BUILD_COUNT=0
TOTAL_BUILDS=${#CONFIGS[@]}
FAILED=0

echo "Building $TOTAL_BUILDS firmware variants..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

for config in "${CONFIGS[@]}"; do
    read -r BOARD CPU PSRAM MOS2 DESC <<< "$config"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Board variant number
    if [[ "$BOARD" == "M1" ]]; then
        BOARD_NUM=1
    else
        BOARD_NUM=2
    fi

    OUTPUT_NAME="frank-duke3d_m${BOARD_NUM}_${CPU}_${PSRAM}_${VERSION}.uf2"
    BUILD_FILE="frank-duke3d.uf2"

    echo ""
    echo "[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME"
    echo "  Board: $BOARD | CPU: ${CPU} MHz | PSRAM: ${PSRAM} MHz | $DESC"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake (USB HID enabled for release builds)
    cmake .. \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$CPU" \
        -DPSRAM_SPEED="$PSRAM" \
        -DFLASH_SPEED=66 \
        -DUSB_HID_ENABLED=ON \
        > /dev/null 2>&1

    # Build
    if make -j$(nproc) > /dev/null 2>&1; then
        # Copy output file to release directory
        if [[ -f "$BUILD_FILE" ]]; then
            cp "$BUILD_FILE" "$RELEASE_DIR/$OUTPUT_NAME"
            echo "  ✓ Success → release/$OUTPUT_NAME"
        else
            echo "  ✗ Output file not found"
            FAILED=$((FAILED + 1))
        fi
    else
        echo "  ✗ Build failed"
        FAILED=$((FAILED + 1))
    fi

    cd "$SCRIPT_DIR"
done

# Clean up build directory
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ $FAILED -gt 0 ]]; then
    echo "Release build completed with $FAILED failures!"
    exit 1
else
    echo "Release build complete!"
fi

echo ""
echo "Release files in: $RELEASE_DIR/"
ls -la "$RELEASE_DIR"/frank-duke3d_*_${VERSION}.* 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo "Version: ${MAJOR}.$(printf '%02d' $MINOR)"
