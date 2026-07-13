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
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

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

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        pathLabel = new TextView(this);
        pathLabel.setPadding(dp(16), dp(12), dp(16), dp(4));
        pathLabel.setTextIsSelectable(true);
        root.addView(pathLabel);

        hintLabel = new TextView(this);
        hintLabel.setPadding(dp(16), 0, dp(16), dp(8));
        root.addView(hintLabel);

        listView = new ListView(this);
        LinearLayout.LayoutParams listParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(listView, listParams);

        LinearLayout buttonRow = new LinearLayout(this);
        buttonRow.setOrientation(LinearLayout.HORIZONTAL);
        buttonRow.setPadding(dp(8), dp(8), dp(8), dp(8));

        Button useButton = new Button(this);
        useButton.setText(R.string.folderpicker_button_use);
        useButton.setOnClickListener(v -> finishWithSelection());
        buttonRow.addView(useButton, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

        Button cancelButton = new Button(this);
        cancelButton.setText(R.string.common_cancel);
        cancelButton.setOnClickListener(v -> { setResult(RESULT_CANCELED); finish(); });
        buttonRow.addView(cancelButton, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

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
        hintLabel.setText(SetupActivity.isValidGameFolder(currentDir)
            ? getString(R.string.folderpicker_hint_valid)
            : getString(R.string.folderpicker_hint_invalid));

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

        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, entries);
        listView.setAdapter(adapter);
    }

    private void finishWithSelection() {
        android.content.Intent result = new android.content.Intent();
        result.putExtra(EXTRA_SELECTED_PATH, currentDir.getAbsolutePath());
        setResult(RESULT_OK, result);
        finish();
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }
}
