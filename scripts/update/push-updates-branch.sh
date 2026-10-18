#!/usr/bin/env bash
# GeneralsX @feature Android port 27/09/2026 Replace the `updates` branch with the contents of a
# directory made by publish-update.py. The branch always holds a single commit, so old engine
# binaries never accumulate in the repository's history.
set -euo pipefail
SRC="$(cd "${1:?usage: push-updates-branch.sh <dir made by publish-update.py> [updates|updates-test]}" && pwd)"
# GeneralsX @feature Android port 18/10/2026 The test channel's branch (publish-update.py --channel test).
BRANCH="${2:-updates}"
case "${BRANCH}" in updates|updates-test) ;; *) echo "branch must be updates or updates-test"; exit 1 ;; esac
test -f "${SRC}/manifest.json" || { echo "no manifest in ${SRC}"; exit 1; }
test -f "${SRC}/manifest.json.sig" || echo "note: unsigned -- the launcher ignores it until the 'Sign update' workflow signs it"
REPO="$(git rev-parse --show-toplevel)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
git -C "${WORK}" init -q
cp -r "${SRC}/." "${WORK}/"
SERIAL="$(python3 -c "import json;print(json.load(open('${WORK}/manifest.json'))['serial'])")"
git -C "${WORK}" add -A
git -C "${WORK}" -c user.name="$(git -C "${REPO}" config user.name || echo GeneralsX)" \
    -c user.email="$(git -C "${REPO}" config user.email || echo noreply@example.com)" \
    commit -q -m "update: manifest serial ${SERIAL}"
git -C "${WORK}" push -q --force "$(git -C "${REPO}" remote get-url origin)" HEAD:refs/heads/${BRANCH}
echo "${BRANCH} branch now at serial ${SERIAL}"
