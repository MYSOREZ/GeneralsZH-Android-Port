package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;

// GeneralsX @feature Android port 02/10/2026 The game's text fields edited in a real EditText.
//
// The engine's entry gadget can only append and delete the last character: no cursor, no
// selection, no copy or paste. While the player edits a field, this bar sits at the top of the
// screen (the keyboard covers the bottom) and the field mirrors it -- every change is sent to
// the engine, Done/Enter ends the edit the way Return does on the PC, Back just closes the bar.
// The native half is AndroidTextEditor.cpp, which explains the protocol; this class only shows
// the bar and reports what happens in it. All methods run on the UI thread.
final class TextEditorBar {

    // Keep in step with AndroidTextEditor.cpp.
    static final int FLAG_SECRET = 1;
    static final int FLAG_DIGITS_ONLY = 2;

    interface Listener {
        void onChanged(String text, int serial);
        void onDone(String text, boolean submit, int serial);
    }

    private final Activity activity;
    private final ViewGroup parent;
    private final Listener listener;
    private final Runnable restoreGameFocus;

    private LinearLayout bar;
    private EditText edit;
    private int serial;
    private boolean settingText;

    TextEditorBar(Activity activity, ViewGroup parent, Listener listener, Runnable restoreGameFocus) {
        this.activity = activity;
        this.parent = parent;
        this.listener = listener;
        this.restoreGameFocus = restoreGameFocus;
    }

    boolean isShown() {
        return bar != null && bar.getVisibility() == View.VISIBLE;
    }

    void show(String text, int maxLength, int flags, int serial) {
        this.serial = serial;
        if (bar == null) {
            create();
        }

        int inputType;
        if ((flags & FLAG_DIGITS_ONLY) != 0) {
            inputType = InputType.TYPE_CLASS_NUMBER;
        } else if ((flags & FLAG_SECRET) != 0) {
            inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        } else {
            inputType = InputType.TYPE_CLASS_TEXT;
        }
        if ((flags & FLAG_SECRET) != 0 && (flags & FLAG_DIGITS_ONLY) != 0) {
            inputType = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD;
        }
        edit.setInputType(inputType);
        edit.setFilters(maxLength > 0
            ? new InputFilter[] { new InputFilter.LengthFilter(maxLength) }
            : new InputFilter[0]);

        settingText = true;
        edit.setText(text);
        edit.setSelection(edit.getText().length());
        settingText = false;
        // The keyboard keeps its own idea of the text being composed (the underlined word and its
        // suggestions). Without a restart it re-committed the previous field's text into this
        // one: chat typed and sent, then the lobby name field opened with the chat line in it.
        InputMethodManager restartImm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (restartImm != null) {
            restartImm.restartInput(edit);
        }

        applyCutoutMargins();
        bar.setVisibility(View.VISIBLE);
        bar.bringToFront();
        edit.requestFocus();
        // Asked for once the bar is laid out; an immediate request on a view that was GONE
        // a moment ago is ignored by some keyboards.
        edit.post(() -> {
            InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showSoftInput(edit, InputMethodManager.SHOW_IMPLICIT);
            }
        });
    }

    void hide() {
        if (!isShown()) {
            return;
        }
        InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.hideSoftInputFromWindow(edit.getWindowToken(), 0);
        }
        bar.setVisibility(View.GONE);
        // Nothing of this field may reach the next one (see show()).
        settingText = true;
        edit.setText("");
        settingText = false;
        if (imm != null) {
            imm.restartInput(edit);
        }
        restoreGameFocus.run();
    }

    /** Ends the edit; submit = the player pressed Done/Enter rather than Back. */
    void finish(boolean submit) {
        if (!isShown()) {
            return;
        }
        listener.onDone(edit.getText().toString(), submit, serial);
        hide();
    }

    private void create() {
        bar = new LinearLayout(activity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(0xE6101418);
        int pad = dp(6);
        bar.setPadding(pad, pad, pad, pad);
        // The bar is a separate surface over the game: taps on it must not reach the game.
        bar.setClickable(true);

        edit = new EditText(activity);
        edit.setSingleLine(true);
        // Its own background: the launcher's light theme gave the field a white box, and the text
        // (white, for the dark bar) vanished in it.
        GradientDrawable field = new GradientDrawable();
        field.setColor(0xFF2A2F36);
        field.setStroke(dp(1), 0xFF5C6670);
        field.setCornerRadius(dp(6));
        edit.setBackground(field);
        int fieldPad = dp(8);
        edit.setPadding(fieldPad, fieldPad, fieldPad, fieldPad);
        edit.setTextColor(Color.WHITE);
        edit.setHintTextColor(0xFF9AA0A6);
        edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        // The keyboard's full-screen landscape editor would hide the game behind it.
        edit.setImeOptions(EditorInfo.IME_ACTION_DONE
            | EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        edit.setOnEditorActionListener((v, actionId, event) -> {
            boolean enterKey = event != null
                && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                && event.getAction() == KeyEvent.ACTION_DOWN;
            if (actionId == EditorInfo.IME_ACTION_DONE || enterKey) {
                finish(true);
                return true;
            }
            return false;
        });
        edit.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
            @Override public void afterTextChanged(Editable s) {
                if (!settingText) {
                    listener.onChanged(s.toString(), serial);
                }
            }
        });
        bar.addView(edit, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button done = new Button(activity);
        done.setText(android.R.string.ok);
        done.setOnClickListener(v -> finish(true));
        LinearLayout.LayoutParams doneParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        doneParams.setMarginStart(dp(6));
        bar.addView(done, doneParams);

        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        bar.setVisibility(View.GONE);
        parent.addView(bar, params);
    }

    // Keep the field and the button clear of a camera cutout at either side.
    private void applyCutoutMargins() {
        int left = 0;
        int right = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowInsets insets = parent.getRootWindowInsets();
            DisplayCutout cutout = insets != null ? insets.getDisplayCutout() : null;
            if (cutout != null) {
                left = cutout.getSafeInsetLeft();
                right = cutout.getSafeInsetRight();
            }
        }
        int pad = dp(6);
        bar.setPadding(pad + left, pad, pad + right, pad);
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
