# Publishing an update without a new APK

The launcher checks the repository's `updates` branch -- from **Home → Updates** (on start and
with **Check for updates**) and when the **GeneralsOnline account** screen opens -- and takes two
kinds of update from it. The settings are applied by either check and shown on the account
screen; the engine is downloaded only by the Home check.

- **Settings** (`update/config.json` in the main tree): values the engine reads at startup. Today
  the STUN and TURN server lists (`stun_servers`, `turn_servers`), the PC client checksum for
  cross-play (`pc_exe_crc`, computed with `scripts/update/pc-exe-crc.py`) and the community data
  patch manifest address (`datapack_manifest_url`), and whether the current PC release ends its
  logic checksum with the GeneralsOnline revision tag (`logic_crc_revision`, `1`/`0`; 100126 does
  not -- used only when the launcher has not read it from the data package's PC executable).
  A missing key keeps the value built in.
  The community data patch itself comes from that manifest and is updated on the multiplayer
  screen (GeneralsOnline account → Online game data), which checks it by itself and installs a
  newer one on Wi-Fi; the Updates card only says when a newer one is out. It also computes the PC checksum
  from the PC executable inside that patch, and that number wins over `pc_exe_crc`, so a new PC
  release normally needs nothing published at all; `pc_exe_crc` covers players without the patch.
- **Data package manifest mirror**: while `datapack_manifest_url` points at the updates branch
  (`.../updates/datapack-manifest.json`), `publish-update.py` copies the GeneralsOnline CDN manifest
  there with its fields trimmed -- the CDN's `sha256` starts with a space, and launchers up to 1.3.0
  compare it untrimmed, failing every install with "checksum mismatch". The script downloads the
  package and refuses to publish unless the trimmed digest and size match it. Newer launchers ignore
  a mirror address and read the CDN themselves. The mirror is as current as the last publish: after
  a new GeneralsOnline release, publish settings again so 1.3.0 players see it. Point the key back at
  `https://cdn.playgenerals.online/manifest.json` once the CDN fixes the digest.
- **Support card** (launchers built from 03/10/2026 on): the Help page's "Support the project"
  card comes entirely from `update/support.json` -- its text in every language, and the
  addresses/links. `publish-update.py` copies the file to `support/<digest>.json` (a new name for every content, so
  GitHub's five-minute per-file cache cannot pair a new manifest with the old file) and writes its
  SHA-256 into the signed manifest, so it is as trusted as the manifest. Nothing of it is in the
  APK, so a new or retired address, a reworded text, or a language added or dropped is just a
  settings publish. Format: `text` maps a language tag (`en`, `ru`, `pt-BR`, `isv`, ...) to
  `title`, `body`, `warning`, `copy_hint`, `copied` (`%s` = the entry's label); `entries` is a list
  of `{ "label", "value" }`, where `label` is a string or a per-language object. A value starting
  with `https://` opens in the browser, anything else is copied. The player's language falls back
  to `en`, which the script requires. Deleting `update/support.json` before a publish withdraws
  the card everywhere.
- **Engine**: a newer `libmain.so` / `libmain60.so`. It is downloaded into the app's private
  storage and used from the next game start, instead of the engine inside the APK.

Nothing is used unless it is signed with the update key.

## How it is protected

- `manifest.json` is signed with an ECDSA P-256 key. The launcher carries only the public key
  (`UpdateManager.PUBLIC_KEY_B64`) and refuses a manifest whose signature does not verify.
- The engine files are checked against the SHA-256 and size written in the signed manifest.
- Every manifest has a `serial`; the launcher never accepts a lower one than it has seen, so an
  old signed manifest cannot be replayed to roll players back.
- A downloaded engine runs only if its build number is higher than the APK's own engine
  (`assets/engine_build.txt`, the commit count it was built at) and only on an install whose
  other native libraries are byte-for-byte the ones it was built with (`requires_libs`). A change
  to SDL, OpenAL, DXVK or anything else in `lib/` therefore still needs a new APK; the launcher
  says so.
- An updated engine that twice fails to reach the main menu is dropped, and the APK's own
  engine runs again.

Nothing in the manifest is secret -- the service addresses are public anyway -- so it is signed,
not encrypted. Encryption would need the key inside the APK, where anyone can take it out.

## The key

`update_signing_key.pem` is the private key. Keep it out of the repository. To sign in GitHub
Actions, store its full text (including the `-----BEGIN/END EC PRIVATE KEY-----` lines) as the
repository secret `UPDATE_SIGNING_KEY` (Settings → Secrets and variables → Actions → New
repository secret). If the key is ever lost, a new one means a new APK with its public key.

## Publishing

Settings only (after editing `update/config.json`):

```bash
python3 scripts/update/publish-update.py --out /tmp/upd --key update_signing_key.pem
scripts/update/push-updates-branch.sh /tmp/upd
```

With a new engine, from an APK built by `scripts/build/android/build-dual-hz.sh` on top of the
same libraries players already have:

```bash
python3 scripts/update/publish-update.py --apk GeneralsXZH-android-local.apk \
    --out /tmp/upd --key update_signing_key.pem --note "what changed"
scripts/update/push-updates-branch.sh /tmp/upd
```

Without the key at hand, leave out `--key`, push, and run **Actions → Sign update → Run
workflow**; it signs the manifest on the branch with the secret in a few seconds.

The branch always holds a single commit, so old engines do not pile up in the history.
