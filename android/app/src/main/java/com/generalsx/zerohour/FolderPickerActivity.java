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

// GeneralsX @build Android port 07/07/2026
//
// A minimal folder browser over plain java.io.File — deliberately NOT the
// system Storage Access Framework picker (ACTION_OPEN_DOCUMENT_TREE): SAF
// hands back a content:// tree the native engine's plain fopen()/chdir()
// calls cannot use directly, which would force copying the ~2-3 GB game
// data into app storage before every launch. Requires the "All files
// access" permission (MANAGE_EXTERNAL_STORAGE, granted from SetupActivity)
// so the resulting path is a real filesystem path the engine can chdir()
// into wherever the user actually put their files — Downloads, an SD card,
// wherever a normal file manager or USB cable already reaches.

package com.generalsx.zerohour;

import android.app.Activity;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.google.android.material.button.MaterialButton;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

public class FolderPickerActivity extends Activity {

    static final String EXTRA_SELECTED_PATH = "selected_path";

    private File currentDir;
    private TextView pathLabel;
    private TextView hintLabel;
    private ListView listView;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.folderpicker_title);

        File start = Environment.getExternalStorageDirectory();
        currentDir = (start != null && start.isDirectory()) ? start : new File("/storage/emulated/0");

        // GeneralsX @feature Android port launcher-ui-refresh 06/09/2026 The
        // browser's own verdict on the folder you are standing in ("this one
        // has the archives" / "keep looking") is the only thing on this
        // screen worth reading, so it gets the semantic status color and the
        // path above it gets the monospace treatment. Rows are padded to a
        // 48dp target -- simple_list_item_1's default is shorter than that on
        // a dense screen, and this list is scrolled with a thumb.
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(LauncherUi.color(this, R.color.gzh_background));

        int pad = LauncherUi.dp(this, LauncherUi.SPACE_4);

        pathLabel = new TextView(this);
        pathLabel.setTextAppearance(R.style.Gzh_Text_BodySmall);
        pathLabel.setTextColor(LauncherUi.color(this, R.color.gzh_on_surface_variant));
        pathLabel.setTypeface(android.graphics.Typeface.MONOSPACE);
        pathLabel.setPadding(pad, pad, pad, LauncherUi.dp(this, LauncherUi.SPACE_1));
        pathLabel.setTextIsSelectable(true);
        root.addView(pathLabel);

        hintLabel = new TextView(this);
        hintLabel.setTextAppearance(R.style.Gzh_Text_Title);
        hintLabel.setPadding(pad, 0, pad, LauncherUi.dp(this, LauncherUi.SPACE_3));
        root.addView(hintLabel);

        listView = new ListView(this);
        listView.setDivider(null);
        LinearLayout.LayoutParams listParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(listView, listParams);

        LinearLayout buttonRow = new LinearLayout(this);
        buttonRow.setOrientation(LinearLayout.HORIZONTAL);
        buttonRow.setPadding(LauncherUi.dp(this, LauncherUi.SPACE_3),
            LauncherUi.dp(this, LauncherUi.SPACE_2),
            LauncherUi.dp(this, LauncherUi.SPACE_3),
            LauncherUi.dp(this, LauncherUi.SPACE_3));

        MaterialButton cancelButton = LauncherUi.button(buttonRow, LauncherUi.BUTTON_TEXT,
            getString(R.string.common_cancel),
            () -> { setResult(RESULT_CANCELED); finish(); });
        cancelButton.setLayoutParams(new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

        MaterialButton useButton = LauncherUi.button(buttonRow, LauncherUi.BUTTON_FILLED,
            getString(R.string.folderpicker_button_use), this::finishWithSelection);
        useButton.setLayoutParams(new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.MATCH_PARENT, 2f));

        root.addView(buttonRow);
        setContentView(root);
        InsetUtil.applySafeInsets(root);

        listView.setOnItemClickListener((AdapterView<?> parent, View view, int position, long id) -> {
            String name = (String) parent.getItemAtPosition(position);
            if (getString(R.string.folderpicker_up_entry).equals(name)) {
                File parentDir = currentDir.getParentFile();
                if (parentDir != null && parentDir.canRead()) {
                    currentDir = parentDir;
                    refresh();
                }
                return;
            }
            File next = new File(currentDir, name);
            if (next.isDirectory()) {
                currentDir = next;
                refresh();
            }
        });

        refresh();
    }

    private void refresh() {
        pathLabel.setText(currentDir.getAbsolutePath());
        boolean looksRight = SetupActivity.isValidGameFolder(currentDir);
        hintLabel.setText(looksRight
            ? getString(R.string.folderpicker_hint_valid)
            : getString(R.string.folderpicker_hint_invalid));
        hintLabel.setTextColor(LauncherUi.color(this,
            looksRight ? R.color.gzh_status_ok : R.color.gzh_on_surface_variant));

        List<String> entries = new ArrayList<>();
        if (currentDir.getParentFile() != null) {
            entries.add(getString(R.string.folderpicker_up_entry));
        }
        File[] children = currentDir.listFiles();
        if (children != null) {
            List<File> dirs = new ArrayList<>();
            for (File f : children) {
                if (f.isDirectory() && !f.isHidden()) {
                    dirs.add(f);
                }
            }
            Collections.sort(dirs, Comparator.comparing(File::getName, String.CASE_INSENSITIVE_ORDER));
            for (File d : dirs) {
                entries.add(d.getName());
            }
        } else {
            Toast.makeText(this, R.string.folderpicker_toast_cant_read, Toast.LENGTH_LONG).show();
        }

        ArrayAdapter<String> adapter = new ArrayAdapter<String>(
                this, android.R.layout.simple_list_item_1, entries) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                TextView row = (TextView) super.getView(position, convertView, parent);
                row.setTextColor(LauncherUi.color(FolderPickerActivity.this, R.color.gzh_on_surface));
                row.setMinHeight(LauncherUi.dp(FolderPickerActivity.this, LauncherUi.TOUCH_TARGET));
                row.setGravity(Gravity.CENTER_VERTICAL);
                int side = LauncherUi.dp(FolderPickerActivity.this, LauncherUi.SPACE_4);
                row.setPadding(side, 0, side, 0);
                return row;
            }
        };
        listView.setAdapter(adapter);
    }

    private void finishWithSelection() {
        android.content.Intent result = new android.content.Intent();
        result.putExtra(EXTRA_SELECTED_PATH, currentDir.getAbsolutePath());
        setResult(RESULT_OK, result);
        finish();
    }
}
