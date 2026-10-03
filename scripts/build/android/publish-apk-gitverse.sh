#!/usr/bin/env bash
# Mirror APKs to GitVerse, for players who cannot reach GitHub -- as GitVerse releases.
#
#   publish-apk-gitverse.sh apk/<name>.apk     test build: the rolling pre-release "test-build"
#                                              holds exactly this APK (the previous one is removed)
#   publish-apk-gitverse.sh --release <ver>    duplicate the GitHub release v<ver>: same title,
#                                              notes (docs/releases/v<ver>/notes.md) and APK
#                                              (the one file in apk/)
#
# Prints the direct download link of the uploaded APK.
#
# Release assets are served from /api/attachments/<uuid> as the real file, so the APK goes up
# bare. (This script used to push a .zip to an "apk" branch: GitVerse's raw-file endpoint sits
# behind a bot filter, and phone browsers saved a broken .apk from it.)
#
# Needs GITVERSE_TOKEN (personal access token with write access to the repository) and
# GITVERSE_REPO (owner/name). The token travels only in request headers, never in a URL, a git
# config or the output. API: https://gitverse.ru/docs/developers/public-api -- a release is
# created on an existing tag, so the tag is pushed first (onto the GitVerse repository's own
# default branch: the mirror holds no source, see its README).
set -euo pipefail

: "${GITVERSE_TOKEN:?GITVERSE_TOKEN is not set}"
: "${GITVERSE_REPO:?GITVERSE_REPO is not set (owner/name)}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
API="https://api.gitverse.ru/repos/${GITVERSE_REPO}"
GIT_URL="https://gitverse.ru/${GITVERSE_REPO}.git"
OWNER="${GITVERSE_REPO%%/*}"

if [ "${1:-}" = "--release" ]; then
    VERSION="${2:?usage: $0 --release <version>}"
    TAG="v${VERSION}"
    TITLE="v${VERSION}"
    NOTES="${REPO}/docs/releases/v${VERSION}/notes.md"
    [ -f "${NOTES}" ] || { echo "no release notes at ${NOTES}"; exit 1; }
    shopt -s nullglob
    APKS=("${REPO}"/apk/*.apk)
    [ "${#APKS[@]}" -eq 1 ] || { echo "expected exactly one APK in apk/, found ${#APKS[@]}"; exit 1; }
    APK="${APKS[0]}"
    ASSET_NAME="GeneralsXZH-android-v${VERSION}.apk"
    PRERELEASE=false
else
    APK="${1:?usage: $0 apk/<name>.apk | --release <version>}"
    [ -f "${APK}" ] || { echo "no such file: ${APK}"; exit 1; }
    TAG="test-build"
    ASSET_NAME="$(basename "${APK}")"
    TITLE="Test build: ${ASSET_NAME%.apk}"
    NOTES=""
    PRERELEASE=true
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

api() {  # api <method> <path> [curl args...] -> body in ${WORK}/resp.json, prints the HTTP status
    local method="$1" path="$2"
    shift 2
    local code i
    for i in 1 2 3 4; do
        code="$(curl -s -o "${WORK}/resp.json" -w '%{http_code}' -X "${method}" \
            -H "Authorization: Bearer ${GITVERSE_TOKEN}" \
            -H "Accept: application/vnd.gitverse.object+json;version=1" \
            "$@" "${API}${path}" || true)"
        # 000: the connection dropped (it does, through some proxies); 5xx: retry as well.
        case "${code}" in 000|5??) sleep $((i * 3)) ;; *) break ;; esac
    done
    echo "${code}"
}
json() { python3 -c "import json,sys; d=json.load(open('${WORK}/resp.json')); $1"; }

# 1. The tag, on the mirror's default branch (created there if it is missing).
AUTH="$(printf '%s:%s' "${OWNER}" "${GITVERSE_TOKEN}" | base64 -w0)"
gv_git() { GIT_TERMINAL_PROMPT=0 git -C "${WORK}/git" -c http.extraHeader="Authorization: Basic ${AUTH}" "$@"; }
git init -q "${WORK}/git"
if ! gv_git ls-remote --tags "${GIT_URL}" "refs/tags/${TAG}" | grep -q .; then
    gv_git fetch -q --depth 1 "${GIT_URL}" HEAD
    gv_git tag "${TAG}" FETCH_HEAD
    gv_git push -q "${GIT_URL}" "refs/tags/${TAG}"
fi

# 2. The release: created, or updated if it already exists.
BODY_FILE="${WORK}/release.json"
python3 - "${TAG}" "${TITLE}" "${NOTES}" "${PRERELEASE}" > "${BODY_FILE}" <<'EOF'
import json, sys
tag, title, notes_path, pre = sys.argv[1:5]
body = open(notes_path, encoding="utf-8").read() if notes_path else (
    "Current test build of the Android port. Install the APK below.\n\n"
    "Releases and source: https://github.com/MYSOREZ/GeneralsZH-Android-Port")
print(json.dumps({"tag_name": tag, "name": title, "body": body,
                  "draft": False, "prerelease": pre == "true"}))
EOF
code="$(api GET "/releases/tags/${TAG}")"
if [ "${code}" = 200 ]; then
    RID="$(json 'print(d["id"])')"
    code="$(api PATCH "/releases/${RID}" -H "Content-Type: application/json" -d "@${BODY_FILE}")"
    [ "${code}" = 200 ] || { echo "updating release ${TAG} failed: HTTP ${code}"; cat "${WORK}/resp.json"; exit 1; }
else
    code="$(api POST "/releases" -H "Content-Type: application/json" -d "@${BODY_FILE}")"
    [ "${code}" = 201 ] || { echo "creating release ${TAG} failed: HTTP ${code}"; cat "${WORK}/resp.json"; exit 1; }
    RID="$(json 'print(d["id"])')"
fi

# 3. The APK: whatever the release held before goes (a test release holds one build; a release
#    being re-published replaces its APK), then the APK is uploaded.
code="$(api GET "/releases/${RID}/assets")"
if [ "${code}" = 200 ]; then
    for AID in $(json 'print(" ".join(str(a["id"]) for a in d))'); do
        api DELETE "/releases/${RID}/assets/${AID}" > /dev/null
    done
fi
code="$(api POST "/releases/${RID}/assets?name=${ASSET_NAME}" \
    -F "attachment=@${APK};type=application/vnd.android.package-archive")"
[ "${code}" = 201 ] || { echo "uploading ${ASSET_NAME} failed: HTTP ${code}"; cat "${WORK}/resp.json"; exit 1; }
SIZE="$(json 'print(d["size"])')"
[ "${SIZE}" = "$(stat -c %s "${APK}")" ] || { echo "uploaded size ${SIZE} differs from ${APK}"; exit 1; }

json 'print(d["browser_download_url"])'
