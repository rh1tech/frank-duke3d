#!/bin/bash
#
# release.sh - Build all release variants of frank-duke3d
#
# Creates firmware files for each board variant (M1, M2) at each clock speed:
#   - Medium overclock: 378 MHz CPU, 133 MHz PSRAM (126 MHz actual)
#   - Max overclock: 504 MHz CPU, 166 MHz PSRAM (168 MHz actual)
#
# Output formats:
#   - UF2: frank-duke3d_mX_Y_Z_A_BB.uf2 (standard Pico firmware)
#   - MOS2: frank-duke3d_mX_Y_Z_A_BB.m1p2/m2p2 (Murmulator OS 2)
#
# X  = Board variant (1 or 2)
# Y  = CPU clock in MHz
# Z  = PSRAM clock in MHz (target)
# A  = Major version
# BB = Minor version (zero-padded)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Version file
VERSION_FILE="version.txt"

# Read last version or initialize
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

# Calculate next version (for default suggestion)
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

# Interactive version input
echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    frank-duke3d Release Builder                  │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}

# Parse version (handle both "1.00" and "1 00" formats)
if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

# Remove leading zeros for arithmetic, then re-pad
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

# Validate
if [[ $MAJOR -lt 1 ]]; then
    echo -e "${RED}Error: Major version must be >= 1${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

# Format version string
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"

# Save new version
echo "$MAJOR $MINOR" > "$VERSION_FILE"

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

# Build configurations: "BOARD CPU_SPEED PSRAM_SPEED MOS2 DESCRIPTION"
# Tested working configurations:
#   378 MHz CPU + 133 MHz PSRAM = 126 MHz actual (medium)
#   504 MHz CPU + 166 MHz PSRAM = 168 MHz actual (max stable)
CONFIGS=(
    # Standard UF2 builds
    "M1 378 133 OFF medium-oc"
    "M1 504 166 OFF max-oc"
    "M2 378 133 OFF medium-oc"
    "M2 504 166 OFF max-oc"
    # MOS2 builds (Murmulator OS 2)
    "M1 378 133 ON mos2-medium"
    "M1 504 166 ON mos2-max"
    "M2 378 133 ON mos2-medium"
    "M2 504 166 ON mos2-max"
)

BUILD_COUNT=0
TOTAL_BUILDS=${#CONFIGS[@]}

echo ""
echo -e "${YELLOW}Building $TOTAL_BUILDS firmware variants...${NC}"
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

    # Determine file extension and output name
    if [[ "$MOS2" == "ON" ]]; then
        if [[ "$BOARD" == "M1" ]]; then
            EXT="m1p2"
        else
            EXT="m2p2"
        fi
        OUTPUT_NAME="frank-duke3d_m${BOARD_NUM}_${CPU}_${PSRAM}_${VERSION}.${EXT}"
        BUILD_FILE="frank-duke3d.${EXT}"
    else
        EXT="uf2"
        OUTPUT_NAME="frank-duke3d_m${BOARD_NUM}_${CPU}_${PSRAM}_${VERSION}.uf2"
        BUILD_FILE="frank-duke3d.uf2"
    fi

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU} MHz | PSRAM: ${PSRAM} MHz | $DESC"

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
        -DMOS2="$MOS2" \
        > /dev/null 2>&1

    # Build
    if make -j8 > /dev/null 2>&1; then
        # Copy output file to release directory
        if [[ -f "$BUILD_FILE" ]]; then
            cp "$BUILD_FILE" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ Output file not found${NC}"
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
    fi

    cd "$SCRIPT_DIR"
done

# Clean up build directory
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""
echo "Release files in: $RELEASE_DIR/"
echo ""
echo -e "${CYAN}Standard UF2 files:${NC}"
ls -la "$RELEASE_DIR"/frank-duke3d_*_${VERSION}.uf2 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}' || echo "  (none)"
echo ""
echo -e "${CYAN}MOS2 files (Murmulator OS 2):${NC}"
ls -la "$RELEASE_DIR"/frank-duke3d_*_${VERSION}.m?p2 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}' || echo "  (none)"
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
