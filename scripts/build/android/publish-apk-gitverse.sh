#!/usr/bin/env bash
# Mirror APKs to GitVerse, for players who cannot reach GitHub.
#
#   publish-apk-gitverse.sh apk/<name>.apk     test build: the branch "apk" holds this APK plus
#                                              every build in apk/testers/ (builds people are
#                                              testing, kept until the owner removes them) --
#                                              one commit, replaced each time, so builds that
#                                              were removed never pile up in its history
#   publish-apk-gitverse.sh --release <ver>    duplicate the GitHub release v<ver> as a GitVerse
#                                              release: same title, notes
#                                              (docs/releases/v<ver>/notes.md) and APK (the one
#                                              file in apk/)
#
# Prints the direct download link of the APK.
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
    # Test builds: one commit on the branch "apk" holding the bare APK, pushed over the last one.
    BRANCH="${GITVERSE_BRANCH:-apk}"
    NAME="$(basename "${APK}")"
    WORK="$(mktemp -d)"
    trap 'rm -rf "${WORK}"' EXIT
    git -C "${WORK}" init -q -b "${BRANCH}"
    cp "${APK}" "${WORK}/${NAME}"
    printf 'Current GeneralsXZH Android test build: %s\n' "${NAME}" > "${WORK}/README.md"
    # Builds handed to testers stay downloadable at the same address on every publish.
    shopt -s nullglob
    TESTERS=("${REPO}"/apk/testers/*.apk)
    if [ "${#TESTERS[@]}" -gt 0 ]; then
        mkdir -p "${WORK}/testers"
        cp "${TESTERS[@]}" "${WORK}/testers/"
        printf '\nBuilds people are testing: testers/\n' >> "${WORK}/README.md"
    fi
    git -C "${WORK}" add -A
    git -C "${WORK}" -c user.name="GeneralsXZH build" -c user.email="noreply@example.invalid" \
        commit -q -m "APK: ${NAME}"
    AUTH="$(printf '%s:%s' "${OWNER}" "${GITVERSE_TOKEN}" | base64 -w0)"
    for i in 1 2 3 4; do
        GIT_TERMINAL_PROMPT=0 git -C "${WORK}" -c http.extraHeader="Authorization: Basic ${AUTH}" \
            -c http.postBuffer=157286400 push -q --force "${GIT_URL}" "${BRANCH}:${BRANCH}" && break
        [ "${i}" = 4 ] && { echo "push to GitVerse failed"; exit 1; }
        sleep $((i * 5))
    done
    echo "https://gitverse.ru/api/repos/${GITVERSE_REPO}/raw/branch/${BRANCH}/${NAME}"
    exit 0
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

# 3. The APK. A release must never stand empty while players are looking at it, so:
#    - an asset with the same name and size is the same build (republishing notes): kept as is;
#    - otherwise the new APK goes up first, and only then are the other assets removed. An asset
#      with the same name but another size has to go first (names are unique in a release).
LOCAL_SIZE="$(stat -c %s "${APK}")"
code="$(api GET "/releases/${RID}/assets")"
EXISTING="[]"
[ "${code}" = 200 ] && EXISTING="$(cat "${WORK}/resp.json")"
SAME_ID="$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); m=[a for a in d if a['name']==sys.argv[2] and a['size']==int(sys.argv[3])]; print(m[0]['id'] if m else '')" "${EXISTING}" "${ASSET_NAME}" "${LOCAL_SIZE}")"
if [ -n "${SAME_ID}" ]; then
    KEEP_ID="${SAME_ID}"
    printf '%s' "${EXISTING}" > "${WORK}/resp.json"
    json "a=[x for x in d if x['id']==${KEEP_ID}][0]; print(a['browser_download_url'])" > "${WORK}/link.txt"
else
    CLASH_ID="$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); m=[a for a in d if a['name']==sys.argv[2]]; print(m[0]['id'] if m else '')" "${EXISTING}" "${ASSET_NAME}")"
    [ -n "${CLASH_ID}" ] && api DELETE "/releases/${RID}/assets/${CLASH_ID}" > /dev/null
    code="$(api POST "/releases/${RID}/assets?name=${ASSET_NAME}" \
        -F "attachment=@${APK};type=application/vnd.android.package-archive")"
    [ "${code}" = 201 ] || { echo "uploading ${ASSET_NAME} failed: HTTP ${code}"; cat "${WORK}/resp.json"; exit 1; }
    [ "$(json 'print(d["size"])')" = "${LOCAL_SIZE}" ] || { echo "uploaded size differs from ${APK}"; exit 1; }
    KEEP_ID="$(json 'print(d["id"])')"
    json 'print(d["browser_download_url"])' > "${WORK}/link.txt"
fi
for AID in $(python3 -c "import json,sys; print(' '.join(str(a['id']) for a in json.loads(sys.argv[1])))" "${EXISTING}"); do
    [ "${AID}" = "${KEEP_ID}" ] || api DELETE "/releases/${RID}/assets/${AID}" > /dev/null
done

# The /api/attachments/<uuid> link: it serves the file to anyone. The release page's
# /releases/download/<tag>/<name> form answers 404 to a visitor who is not signed in.
cat "${WORK}/link.txt"
