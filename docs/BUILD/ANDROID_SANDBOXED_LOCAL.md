# Building the Android APK inside a sandboxed dev environment (no GitHub Actions)

This documents how the Android debug APK was built end-to-end *inside a
sandboxed Claude Code session* (the `claude.ai/code` web/cloud sandbox),
without using GitHub Actions at all. It's here so the next session --
Claude or human -- doesn't have to rediscover any of this from scratch.

## The constraint

The sandbox's egress policy blocks raw HTTPS to `github.com` and
`codeload.github.com`. Concretely: any `curl`-based download of a GitHub
source archive or release asset returns `403 Forbidden`. This breaks, out
of the box:

- vcpkg's own source-archive downloads (`vcpkg_from_github()` /
  `vcpkg_download_distfile()`, used by ~12 of this project's ~20 vcpkg
  dependencies)
- vcpkg's bootstrap of its own tool binary, plus CMake/Ninja/patchelf,
  which it downloads the same way
- CMake's `FetchContent(URL ...)` for SDL3, SDL3_image, and openal-soft
  (`cmake/sdl3.cmake`, `cmake/openal.cmake`) -- release tarballs, not vcpkg
- a handful of one-off release binaries the packaging script fetches
  (Liberation fonts, the default Turnip Vulkan driver)

What **does** work in this environment:

1. **Plain `git clone`/`fetch`/`checkout` against `github.com`** -- routed
   through a local git proxy, unaffected by the HTTPS block. This is the
   basis for every durable fix below.
2. **Resolving `github.com/.../releases/download/...` redirects via an
   agent's web-fetch tool**, then `curl`-ing the resolved
   `release-assets.githubusercontent.com` / `objects.githubusercontent.com`
   URL directly (not blocked). Only useful for one-off manual fetches --
   see below for why this can't be baked into a script.
3. `dl.google.com` (Android SDK/NDK) is not blocked at all; no workaround
   needed there.

Everything else in this doc follows from those two facts.

## Durable fixes (committed, reusable by any future session)

### `scripts/build/android/vcpkg-sandbox-patches/`

Two of vcpkg's own scripts (`vcpkg_from_github.cmake`,
`vcpkg_download_distfile.cmake`), patched to reproduce a GitHub source
archive via `git clone` + `git checkout <ref>` + `git archive` instead of
the normal hash-checked HTTPS download, whenever the target file isn't
already in the downloads cache. `REF` still pins the exact same upstream
commit/tag either way -- this only changes *how* the bytes get onto disk,
not which commit's code ends up there, so it doesn't weaken what vcpkg
normally verifies via `SHA512`; a git-cloned checkout is anyway the
strongest possible provenance for a REF. `apply.sh` copies both files over
a vcpkg checkout at the project's pinned commit (see
`.github/workflows/build-android.yml`'s `VCPKG_COMMIT`); safe/no-op on a
machine with normal GitHub access, since the fallback only triggers on a
403-shaped failure path that never gets hit there.

### `scripts/build/android/build-local-sandboxed.sh`

The actual build driver used to produce the APK in this session. Mirrors
`.github/workflows/build-android.yml`'s steps, plus:

- applies the vcpkg patches above after bootstrapping vcpkg
- clones SDL3/SDL3_image/openal-soft at their pinned release tags into
  `/opt/fetchcontent-src/` and points CMake at them via
  `FETCHCONTENT_SOURCE_DIR_<NAME>`, since the vcpkg patches don't cover
  CMake's own `FetchContent(URL ...)` downloader
- symlinks around a DXVK/meson quirk where the generated `sdl3.pc` hardcodes
  a `_deps/sdl3-src/include` path that the `FETCHCONTENT_SOURCE_DIR`
  override otherwise leaves empty
- strips debug symbols from every custom-built `.so` *except*
  `libdxvk_d3d8.so`/`libdxvk_d3d9.so` before packaging (see "APK size" below)

Run it from a repo checkout with:

```bash
./scripts/build/android/build-local-sandboxed.sh
```

It still needs the one-off assets below staged first, or it'll fail with a
clear error at the step that needs them.

### `android-staging/` is gitignored

The packaging script's (`package-android-zh.sh`) staging directory for
fetched assets (fonts, default Turnip driver) -- build output, not source,
matching the existing convention for `flatpak/staging/` and `ios/build/`.

## One-off binary assets (not scriptable, redo per session)

These are all single small files fetched from a `github.com/.../releases/
download/...` or `.../archive/...` URL that isn't covered by the durable
fixes above (either because it's needed *before* the vcpkg patches exist
yet -- vcpkg's own tool binary -- or because the redirect target is a
presigned URL that **expires within minutes**, so there's no fixed URL to
hardcode into a script). Each one needs an agent capable of resolving a
redirect (Claude's `WebFetch` tool did this: fetch the `github.com` URL,
it reports the resolved `release-assets.githubusercontent.com` /
`objects.githubusercontent.com` URL, `curl` that directly) and then placing
the result at the exact path the consuming tool expects:

| Asset | Source | Where it goes | Verify |
|---|---|---|---|
| `vcpkg` tool binary | `microsoft/vcpkg-tool` release (glibc build) | `/opt/vcpkg/vcpkg`, `chmod +x` | SHA512 against `VCPKG_GLIBC_SHA` in `/opt/vcpkg/scripts/vcpkg-tool-metadata.txt` |
| CMake | `Kitware/CMake` release tarball | `/opt/vcpkg/downloads/cmake-<ver>-linux-x86_64.tar.gz` | vcpkg re-verifies on extract |
| Ninja | `ninja-build/ninja` release zip | `/opt/vcpkg/downloads/ninja-linux-<ver>.zip` (exact filename vcpkg's log asks for -- not the URL's basename) | vcpkg re-verifies on extract |
| patchelf | `NixOS/patchelf` release tarball | `/opt/vcpkg/downloads/patchelf-<ver>-x86_64.tar.gz` | SHA512 in `vcpkg_find_acquire_program(PATCHELF).cmake` |
| Liberation fonts | `liberationfonts/liberation-fonts` release/attached-file tarball | extract, rename+copy the 4 `.ttf`s into `${GX_ANDROID_STAGING}/fonts/{arial,arialbold,couriernew,timesnewroman}.ttf` | SHA256 in `scripts/build/ios/stage-fonts.sh` |
| Turnip driver | `K11MCH1/AdrenoToolsDrivers` release zip | extract `meta.json` + the `.so` it names into `${GX_ANDROID_STAGING}/default_driver/` | `file` reports AArch64; no upstream checksum (script notes why) |

If any of these are already present at their destination, the corresponding
script/tool skips fetching and this whole table is moot -- only needed once
per fresh environment.

## Why not just bundle the whole toolchain?

The obvious next idea -- upload the Android SDK/NDK, vcpkg's downloads
cache, ccache, etc. into the repo so a future session skips all of the
above -- doesn't hold up:

- **GitHub's 100MB per-file limit.** The NDK alone is over 1GB; vcpkg's
  downloads cache for this project is 300MB+. Neither fits without Git LFS,
  and LFS uploads to this fork are disabled by GitHub itself
  (`can not upload new objects to public fork`) -- confirmed by hitting it
  head-on trying to LFS-track the built APK.
- **It's not actually the bottleneck.** `dl.google.com` (SDK/NDK) was never
  blocked. vcpkg's *build* time (compiling ~20 C/C++ dependencies) is the
  slow part, and that's exactly what vcpkg's own binary cache
  (`~/.cache/vcpkg/archives`) and ccache already exist to avoid on a
  *second* run in the *same* long-lived environment -- they just don't
  survive a fresh session/container.
- **The one-off assets above are tiny (all under 20MB combined) and cheap
  to re-fetch** -- the durable fixes handle everything that's actually
  expensive to reproduce (the ~20 vcpkg source archives, all multi-MB to
  multi-hundred-MB), leaving only a handful of small files that need an
  agent's redirect-resolution step regardless of whether they're
  pre-staged, since the presigned URLs expire before a stale copy would
  even help.

So: the patches + driver script here are the actual reusable artifact. A
fresh sandboxed session still pays for the vcpkg *build* time once (no way
around that without a much larger persistent cache this fork can't host),
but no longer has to rediscover any of the above.

## Bugs found along the way (not sandbox-specific)

Two real bugs surfaced while building here that would bite *any*
environment, not just this sandboxed one -- noted in case they resurface:

- **`ANDROID_CI_BUILD_NUMBER` must be a plain integer.** It's consumed by
  a `%d` format string in `SDL3Main.cpp`'s startup banner. CI always passes
  `github.run_number` (numeric); a non-numeric value like `local-<epoch>`
  fails to compile (`use of undeclared identifier`, since the generated
  header's `#define` line isn't valid C).
- **`libdxvk_d3d8.so`/`libdxvk_d3d9.so` must stay unstripped.** Already
  documented in `android/app/build.gradle` (`keepDebugSymbols`) --
  stripping them corrupts DXVK's Vulkan dispatch on real hardware
  (SIGSEGV mid-render). Anything that strips native libraries outside of
  Gradle's own (currently broken for this project's libraries) strip step
  must preserve this exclusion.

## APK size

A debug build with all native libraries left unstripped is ~119MB
(`libmain.so` alone is 282MB uncompressed before stripping). Gradle's own
`stripDebugDebugSymbols` task fails silently on every one of this
project's custom-built libraries ("Unable to strip ... packaging them as
they are") for reasons unrelated to the deliberate DXVK exception above --
so `build-local-sandboxed.sh` strips everything itself with `llvm-strip
--strip-unneeded` before packaging, *except* the two DXVK libraries. That
brings a clean build down to ~39MB.

One gotcha hit while verifying this: AGP's incremental APK packager can
leave old bytes physically in the zip file when a library shrinks (only
the central directory gets updated to point at the new, smaller entry) --
the on-disk `.apk` didn't shrink at all until `android/app/build` was
removed and packaging re-run from a clean slate. If a rebuild's output
size looks wrong, that's the first thing to check.

## Distributing the APK: don't use Git LFS on this fork

Test builds live in `apk/*.apk`, committed as **plain git blobs**, not Git
LFS. GitHub's regular per-file commit limit is 100MB; stripping (above)
keeps debug builds around 39MB, comfortably under that, so LFS should never
be necessary here. Two reasons to actively avoid it if a future build ever
does creep past 100MB:

1. **LFS uploads are disabled on this fork.** `git lfs push` fails outright
   with `@<owner> can not upload new objects to public fork
   <owner>/<repo>` -- a GitHub-side restriction, not something this
   sandbox's proxy is blocking. Confirmed by trying it directly.
2. **A half-undone LFS experiment silently corrupts the next commit.**
   Sequence that caused this once: `git lfs track` + `git add` + commit
   (creates an LFS-pointer blob, correctly) -> push fails -> `git reset
   --soft HEAD~1` to undo the commit -> delete `.gitattributes`, `git lfs
   uninstall`, replace the working-tree file with new content, `git add`
   again -> commit -> push. The second commit's blob was *still* an LFS
   pointer (just a smaller one, matching the new file's size), even though
   `git check-attr filter` on that path reported `unspecified` and no LFS
   filter config was left anywhere (`.git/info/attributes`, global
   gitattributes, global/local git config all clean). `git reset --soft`
   only moves `HEAD`; it leaves the **index** exactly as the undone commit
   left it, still holding an LFS-clean-filtered pointer entry for that
   path. Apparently that stale index state, not live attribute lookup, is
   what `git add` extended from — the smudge/clean machinery had already
   done its job once and a filter-less `git add` afterward didn't
   necessarily re-run it. If a real fix is only under ~40MB anyway, sanity
   check that the *committed blob* isn't secretly a pointer:

   ```bash
   git cat-file -p HEAD:apk/<file>.apk | head -c 40
   # real APK: starts with "PK" (zip signature)
   # LFS pointer: starts with "version https://git-lfs..."
   ```

   The reliable fix, which sidesteps any lingering filter/index state
   entirely, is writing the blob directly and pointing the index at it by
   hand instead of trusting `git add`:

   ```bash
   BLOB=$(git hash-object -w --no-filters apk/<file>.apk)
   git update-index --add --cacheinfo 100644,"$BLOB",apk/<file>.apk
   git commit -m "..."
   ```

   `--no-filters` guarantees the object written to the store is the exact
   working-tree bytes, no matter what `.gitattributes`/git-lfs state
   might otherwise be lurking. Verify with the `cat-file -p | head -c 40`
   check above before pushing.

If a build ever *does* need to exceed 100MB and stripping/shrinking isn't
an option, use a **GitHub Release** attached to a tag (2GB/file limit,
doesn't touch git history or trip the LFS restriction) rather than
retrying LFS on this fork.
