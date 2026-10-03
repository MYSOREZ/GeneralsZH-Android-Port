package com.generalsx.zerohour;

import android.content.Context;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// GeneralsX @feature Android port 03/10/2026 The Help page's "Support the project" card (README
// "Support the project"), read from update/support.json.
//
// Nothing of the card is in the APK -- not the addresses, not the text, not the set of languages.
// The file is published with the signed settings (docs/HOWTO/PUBLISH_UPDATE.md): its SHA-256 is in
// the signed manifest, so UpdateManager only keeps a copy that matches it. That signature matters
// more here than anywhere else: an address swapped in transit would send someone's money elsewhere.
// Adding or retiring an address, rewording the text, adding or dropping a language: all of it is a
// settings publish, which reaches every launcher from this one on without a new APK.
//
// Format:
//   { "schema": 1,
//     "text": { "en": { "title", "body", "warning", "copy_hint", "copied" }, "ru": { ... }, ... },
//     "entries": [ { "label": "USDT — TON", "value": "UQ..." },
//                  { "label": { "en": "Card", "ru": "Карта" }, "value": "https://..." } ] }
// Languages are BCP 47 tags ("pt-BR", "zh"); a field missing in the player's language comes from
// "en", then from whichever language has it. A value starting with https:// opens in the browser;
// anything else is an address and is copied. "copied" may contain %s for the entry's label.
//
// No file, an unreadable one, or one with no usable entry: the card is not shown. A launcher that
// has never fetched settings therefore shows nothing rather than an address that may have died.
final class SupportLinks {

    static final String FILE_NAME = "support.json";

    static final class Entry {
        final String label;
        final String value;

        Entry(String label, String value) {
            this.label = label;
            this.value = value;
        }

        boolean isLink() {
            return value.startsWith("https://");
        }
    }

    final String title;
    final String body;
    final String warning;
    final String copyHint;
    final String copied;
    final List<Entry> entries;

    private SupportLinks(String title, String body, String warning, String copyHint, String copied,
                         List<Entry> entries) {
        this.title = title;
        this.body = body;
        this.warning = warning;
        this.copyHint = copyHint;
        this.copied = copied;
        this.entries = entries;
    }

    /** The card's content in the launcher's language, or null when there is nothing to show. */
    static SupportLinks load(Context ctx) {
        File file = new File(UpdateManager.updateDir(ctx), FILE_NAME);
        if (!file.isFile()) {
            return null;
        }
        try {
            JSONObject root = new JSONObject(readText(file));
            Locale locale = launcherLocale(ctx);
            JSONObject text = root.optJSONObject("text");
            if (text == null) {
                text = new JSONObject();
            }
            List<Entry> entries = new ArrayList<>();
            JSONArray list = root.optJSONArray("entries");
            for (int i = 0; list != null && i < list.length(); i++) {
                JSONObject e = list.optJSONObject(i);
                if (e == null) {
                    continue;
                }
                Object labelObj = e.opt("label");
                String label = labelObj instanceof JSONObject
                    ? pick(strings((JSONObject) labelObj), locale)
                    : e.optString("label", "").trim();
                String value = e.optString("value", "").trim();
                if (usable(label, value)) {
                    entries.add(new Entry(label, value));
                }
            }
            String title = field(text, locale, "title");
            if (entries.isEmpty() || title.isEmpty()) {
                return null;
            }
            return new SupportLinks(title, field(text, locale, "body"), field(text, locale, "warning"),
                field(text, locale, "copy_hint"), field(text, locale, "copied"), entries);
        } catch (Exception e) {
            // A malformed file is published by mistake, not by an attacker (it matched the signed
            // manifest); showing no card is the safe reading of it.
            android.util.Log.w("GXSupport", "support.json unreadable", e);
            return null;
        }
    }

    // An entry without a label or value, with whitespace in the value, or with a link that is not
    // https is skipped rather than shown half-formed.
    private static boolean usable(String label, String value) {
        if (label.isEmpty() || value.isEmpty() || value.matches(".*\\s.*")) {
            return false;
        }
        return !value.contains("://") || value.startsWith("https://");
    }

    private static String field(JSONObject text, Locale locale, String name) {
        Map<String, String> byTag = new LinkedHashMap<>();
        for (Iterator<String> it = text.keys(); it.hasNext(); ) {
            String tag = it.next();
            JSONObject t = text.optJSONObject(tag);
            String v = t != null ? t.optString(name, "").trim() : "";
            if (!v.isEmpty()) {
                byTag.put(tag, v);
            }
        }
        return pick(byTag, locale);
    }

    private static Map<String, String> strings(JSONObject byTagJson) {
        Map<String, String> byTag = new LinkedHashMap<>();
        for (Iterator<String> it = byTagJson.keys(); it.hasNext(); ) {
            String tag = it.next();
            String v = byTagJson.optString(tag, "").trim();
            if (!v.isEmpty()) {
                byTag.put(tag, v);
            }
        }
        return byTag;
    }

    /**
     * The value for the launcher's language: exact tag, then language only, then a regional
     * variant of it ("pt-BR" in the file for a plain "pt" launcher), then "en", then any.
     */
    private static String pick(Map<String, String> byTag, Locale locale) {
        String full = locale.toLanguageTag().toLowerCase(Locale.ROOT);
        String lang = locale.getLanguage().toLowerCase(Locale.ROOT);
        String exact = null, language = null, regional = null, english = null;
        for (Map.Entry<String, String> e : byTag.entrySet()) {
            String tag = e.getKey().toLowerCase(Locale.ROOT);
            if (tag.equals(full)) {
                exact = e.getValue();
            } else if (tag.equals(lang)) {
                language = e.getValue();
            } else if (regional == null && tag.startsWith(lang + "-")) {
                regional = e.getValue();
            }
            if (tag.equals("en")) {
                english = e.getValue();
            }
        }
        if (exact != null) return exact;
        if (language != null) return language;
        if (regional != null) return regional;
        if (english != null) return english;
        return byTag.isEmpty() ? "" : byTag.values().iterator().next();
    }

    // The language the launcher's own strings are in (LocaleHelper's override or the system's).
    private static Locale launcherLocale(Context ctx) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            return ctx.getResources().getConfiguration().getLocales().get(0);
        }
        return ctx.getResources().getConfiguration().locale;
    }

    private static String readText(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }
}
