#!/usr/bin/env bash
# Mirror the current test APK to GitVerse, for testers who cannot reach GitHub.
# The APK goes inside a .zip: served bare, phone browsers saved a broken file from GitVerse
# (its raw endpoint sends Content-Disposition: inline behind a bot filter), while the same
# bytes from GitHub installed fine.
#
# Usage: publish-apk-gitverse.sh apk/<name>.apk
#
# Needs GITVERSE_TOKEN (a personal access token with repository write access) and
# GITVERSE_REPO (owner/name) in the environment. The APK goes to the branch "apk" as a
# single commit that replaces the previous one, so the mirror always holds one ~60 MB
# build instead of accumulating every APK in its history. The repository's other
# branches are not touched.
set -euo pipefail

APK="${1:?usage: $0 apk/<name>.apk}"
[ -f "${APK}" ] || { echo "no such file: ${APK}"; exit 1; }
: "${GITVERSE_TOKEN:?GITVERSE_TOKEN is not set}"
: "${GITVERSE_REPO:?GITVERSE_REPO is not set (owner/name)}"
BRANCH="${GITVERSE_BRANCH:-apk}"
OWNER="${GITVERSE_REPO%%/*}"
URL="https://gitverse.ru/${GITVERSE_REPO}.git"
NAME="$(basename "${APK}")"
ZIP_NAME="${NAME%.apk}.zip"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
git -C "${WORK}" init -q -b "${BRANCH}"
# Stored, not deflated: an APK is already compressed.
(cd "$(dirname "${APK}")" && zip -q -0 "${WORK}/${ZIP_NAME}" "${NAME}")
printf 'Current GeneralsXZH Android test build: %s (unzip it and install the .apk inside)\n' "${NAME}" > "${WORK}/README.md"
git -C "${WORK}" add "${ZIP_NAME}" README.md
git -C "${WORK}" -c user.name="GeneralsXZH build" -c user.email="noreply@example.invalid" \
    commit -q -m "APK: ${ZIP_NAME}"

# The token travels in a header, never in the URL, so it is not written to any git config
# or printed in an error message.
AUTH="$(printf '%s:%s' "${OWNER}" "${GITVERSE_TOKEN}" | base64 -w0)"
GIT_TERMINAL_PROMPT=0 git -C "${WORK}" -c http.extraHeader="Authorization: Basic ${AUTH}" \
    -c http.postBuffer=157286400 push -q --force "${URL}" "${BRANCH}:${BRANCH}"

# Direct download link, public for a public repository.
echo "https://gitverse.ru/api/repos/${GITVERSE_REPO}/raw/branch/${BRANCH}/${ZIP_NAME}"
