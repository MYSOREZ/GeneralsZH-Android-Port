#!/usr/bin/env bash
#
# Build an APK carrying both engines, so the launcher's Simulation Rate setting
# has something to switch between.
#
# The tick rate cannot be a runtime option: WWSyncPerSecond is an enum constant,
# baked into every static_assert, array bound and derived timing constant, and
# GameLogic declares m_frameLegacy behind the same macro -- two translation units
# that disagreed about it would disagree about the class layout. So we build the
# engine twice and let GeneralsZHActivity.getLibraries() load one of them.
#
# Pass 1 builds 60 Hz and sets the result aside; pass 2 builds 30 Hz and packages
# both. The 30 Hz build goes last because it produces libmain.so, the default the
# launcher falls back to.
#
# The APK roughly doubles in size, since libmain.so is by far the largest thing in
# it. That is the price of the setting.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
STASH="${REPO}/build/libmain60.so"

echo "=== pass 1 of 2: 60 Hz engine ==="
GX_EXTRA_CMAKE="-DSAGE_HIGH_FPS_SIM=ON" "${SCRIPT_DIR}/build-local-sandboxed.sh"

BUILT="${REPO}/build/android-vulkan/GeneralsMD/Code/Main/libmain.so"
[ -f "${BUILT}" ] || { echo "60 Hz libmain.so missing at ${BUILT}"; exit 1; }
mkdir -p "$(dirname "${STASH}")"
cp "${BUILT}" "${STASH}"
echo "60 Hz engine set aside: ${STASH} ($(du -h "${STASH}" | cut -f1))"

echo "=== pass 2 of 2: 30 Hz engine, then package both ==="
GX_EXTRA_CMAKE="-DSAGE_HIGH_FPS_SIM=OFF" GX_SECOND_GAME_LIB="${STASH}" \
    "${SCRIPT_DIR}/build-local-sandboxed.sh"

echo "=== done: the APK carries libmain.so (30 Hz) and libmain60.so (60 Hz) ==="
