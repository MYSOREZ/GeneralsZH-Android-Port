/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @build Android port 06/07/2026, reworked 07/07/2026
// Thin shell over SDL3's SDLActivity. Responsibilities:
//  1. Name the native libraries to load (libmain.so = the game).
//  2. On launch, extract the small bundled runtime files (fonts/, dxvk.conf,
//     DefaultOptions.ini) from APK assets into the external files dir, which
//     SDL3Main.cpp makes the game's working directory. Game .big archives are
//     NOT bundled — the user picks their own via the GeneralsZH Setup app.
//  3. If no valid game folder is configured yet (checked via SetupActivity's
//     saved preference, mirroring the marker file SDL3Main.cpp reads),
//     redirect to Setup INSTEAD OF calling super.onCreate() — this means
//     libmain.so is never dlopen'd on a misconfigured install, so a missing
//     game data folder can never look like (or mask) a native crash.

package com.generalsx.zerohour;

import android.content.Intent;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class GeneralsZHActivity extends SDLActivity {

    private static final String TAG = "GeneralsZH";

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            // libmain.so — the game itself (z_generals target, android-vulkan
            // preset). Its DT_NEEDED entries (SDL3_image, openal, c++_shared,
            // gamespy) resolve from the same APK; the DXVK d3d8/d3d9
            // libraries are dlopen()ed by the engine at D3D init.
            "main"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // TheSuperHackers @bugfix Android port 07/07/2026 Belt-and-suspenders
        // on top of the manifest's screenOrientation="landscape": a real
        // device log still showed Resolve_Present_BackBuffer_Size catching a
        // portrait-sized window during the Setup -> Launch transition, so the
        // manifest lock alone isn't settling fast enough on every device/OEM
        // skin. Setting it again here in code takes effect before this
        // Activity's window is even measured, closing the gap further.
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        extractBundledRuntime();

        String gamePath = getSavedGamePath();
        boolean haveCustomPath = gamePath != null && SetupActivity.isValidGameFolder(new File(gamePath));
        boolean haveLegacyPath = !haveCustomPath && isValidGameFolder(legacyGameDataDir());

        if (!haveCustomPath && !haveLegacyPath) {
            // Never touch libmain.so on a misconfigured install: redirect to
            // Setup instead of letting SDLActivity load the native library
            // into an app state that can only end in a black screen or a
            // confusing crash the user has no way to diagnose.
            Log.i(TAG, "no valid game folder configured; redirecting to Setup");
            startActivity(new Intent(this, SetupActivity.class));
            finish();
            return;
        }

        // GeneralsX @bugfix Android port 07/07/2026 Apply the fonts/dxvk.conf/
        // DefaultOptions.ini copy-if-missing fix retroactively on every launch,
        // not just when the folder is freshly picked in Setup — an install that
        // already had a custom path saved before this fix shipped would
        // otherwise keep missing fonts/ forever (every button renders with no
        // text; see SetupActivity.copyBundledRuntimeIfMissing for why).
        if (haveCustomPath) {
            File bundledRoot = getExternalFilesDir(null);
            if (bundledRoot != null) {
                SetupActivity.copyBundledRuntimeIfMissing(bundledRoot, gamePath);
            }
        }

        super.onCreate(savedInstanceState);
    }

    private String getSavedGamePath() {
        return SetupActivity.getSavedGamePath(this);
    }

    // Legacy convention from before the in-app Setup flow existed (an adb
    // push into <external>/GameData) — still honored so nothing breaks for
    // anyone who already has files there.
    private File legacyGameDataDir() {
        File root = getExternalFilesDir(null);
        return root != null ? new File(root, "GameData") : null;
    }

    private boolean isValidGameFolder(File dir) {
        return SetupActivity.isValidGameFolder(dir);
    }

    /**
     * Copy the APK's bundled runtime files into the external files dir
     * (the game's working directory). Existing files are left alone so a
     * user-edited dxvk.conf or replaced font survives updates; delete the
     * file to get a fresh copy on next launch.
     */
    private void extractBundledRuntime() {
        File root = getExternalFilesDir(null);
        if (root == null) {
            Log.e(TAG, "external files dir unavailable; asset extraction skipped");
            return;
        }
        copyAssetTree("gamedata", root);
    }

    private void copyAssetTree(String assetPath, File destRoot) {
        AssetManager assets = getAssets();
        try {
            String[] children = assets.list(assetPath);
            if (children == null || children.length == 0) {
                // Leaf: a real file
                String rel = assetPath.substring("gamedata".length());
                if (rel.startsWith("/")) rel = rel.substring(1);
                if (rel.isEmpty()) return;
                File dest = new File(destRoot, rel);
                if (dest.exists()) return;
                File parent = dest.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    Log.e(TAG, "mkdirs failed for " + parent);
                    return;
                }
                try (InputStream in = assets.open(assetPath);
                     OutputStream out = new FileOutputStream(dest)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                }
                Log.i(TAG, "extracted " + rel);
            } else {
                for (String child : children) {
                    copyAssetTree(assetPath + "/" + child, destRoot);
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "asset extraction failed for " + assetPath, e);
        }
    }
}
