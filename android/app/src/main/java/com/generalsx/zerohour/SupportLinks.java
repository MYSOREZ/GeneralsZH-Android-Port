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
//
// GeneralsX @feature Android port 18/10/2026 Content made of blocks, so anything new -- a list of
// supporters, a goal, a button, a paragraph -- is a settings publish, never a new APK. Launchers
// from 1.4.1 read "card" (the Help page's card) and "popup" (a window shown once per id when the
// launcher opens); older ones keep reading "text"/"entries" above, so both stay in the file.
//   "card":  { "title": T, "blocks": [ B, ... ] }
//   "popup": { "id": "any-string", "title": T, "blocks": [ B, ... ],
//              "buttons": [ { "label": T, "url": "https://..." }, { "label": T, "action": "support" } ] }
//   B = { "type": "heading" | "text" | "note", "text": T }
//     | { "type": "copy", "label": T, "value": "address" }       tap copies the value
//     | { "type": "link", "label": T, "value": "https://..." }  tap opens it
//     | { "type": "button", "label": T, "value": "https://..." }
//     | { "type": "rows", "rows": [ { "left": T, "right": T }, ... ] }   e.g. supporters and amounts
//     | { "type": "progress", "label": T, "value": 40, "max": 200 }    e.g. a monthly goal
//     | { "type": "divider" }
//   T = "text" or { "en": "...", "ru": "...", ... } (same language rules as below).
// An unknown block type is skipped, so a newer file never breaks an older launcher of this kind.
// Changing the popup's id shows the new popup once to everyone, as an announcement.
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

    /** One piece of block content (see the format above). */
    static final class Block {
        final String type;
        final String text;      // heading/text/note; the label of copy/link/button/progress
        final String value;     // copy/link/button
        final List<String[]> rows;  // rows: { left, right }
        final double progress;
        final double max;

        Block(String type, String text, String value, List<String[]> rows, double progress, double max) {
            this.type = type;
            this.text = text;
            this.value = value;
            this.rows = rows;
            this.progress = progress;
            this.max = max;
        }
    }

    /** A window shown once per id (see the format above). */
    static final class Popup {
        final String id;
        final String title;
        final List<Block> blocks;
        final List<String[]> buttons;   // { label, url or "" }, { label, "support" }

        Popup(String id, String title, List<Block> blocks, List<String[]> buttons) {
            this.id = id;
            this.title = title;
            this.blocks = blocks;
            this.buttons = buttons;
        }
    }

    final String title;
    final String body;
    final String warning;
    final String copyHint;
    final String copied;
    final List<Entry> entries;
    /** The Help card as blocks, or null to show the legacy title/body/entries. */
    List<Block> cardBlocks;
    Popup popup;

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
            JSONObject card = root.optJSONObject("card");
            List<Block> cardBlocks = card != null ? blocks(card.optJSONArray("blocks"), locale) : null;
            if (card != null && !loc(card.opt("title"), locale).isEmpty()) {
                title = loc(card.opt("title"), locale);
            }
            Popup popup = popup(root.optJSONObject("popup"), locale);
            if ((entries.isEmpty() && (cardBlocks == null || cardBlocks.isEmpty()) && popup == null)
                    || (title.isEmpty() && popup == null)) {
                return null;
            }
            SupportLinks links = new SupportLinks(title, field(text, locale, "body"), field(text, locale, "warning"),
                field(text, locale, "copy_hint"), field(text, locale, "copied"), entries);
            links.cardBlocks = (cardBlocks != null && !cardBlocks.isEmpty()) ? cardBlocks : null;
            links.popup = popup;
            return links;
        } catch (Exception e) {
            // A malformed file is published by mistake, not by an attacker (it matched the signed
            // manifest); showing no card is the safe reading of it.
            android.util.Log.w("GXSupport", "support.json unreadable", e);
            return null;
        }
    }

    /** A text that is either a plain string or { tag: string } by language. */
    private static String loc(Object o, Locale locale) {
        if (o instanceof JSONObject) {
            return pick(strings((JSONObject) o), locale);
        }
        return o instanceof String ? ((String) o).trim() : "";
    }

    private static List<Block> blocks(JSONArray list, Locale locale) {
        List<Block> out = new ArrayList<>();
        for (int i = 0; list != null && i < list.length(); i++) {
            JSONObject b = list.optJSONObject(i);
            if (b == null) {
                continue;
            }
            String type = b.optString("type", "");
            String label = loc(b.has("text") ? b.opt("text") : b.opt("label"), locale);
            String value = b.optString("value", "").trim();
            switch (type) {
                case "heading":
                case "text":
                case "note":
                    if (!label.isEmpty()) {
                        out.add(new Block(type, label, "", null, 0, 0));
                    }
                    break;
                case "copy":
                    if (usable(label, value) && !value.contains("://")) {
                        out.add(new Block(type, label, value, null, 0, 0));
                    }
                    break;
                case "link":
                case "button":
                    if (!label.isEmpty() && value.startsWith("https://") && !value.matches(".*\\s.*")) {
                        out.add(new Block(type, label, value, null, 0, 0));
                    }
                    break;
                case "rows": {
                    List<String[]> rows = new ArrayList<>();
                    JSONArray r = b.optJSONArray("rows");
                    for (int j = 0; r != null && j < r.length(); j++) {
                        JSONObject row = r.optJSONObject(j);
                        if (row == null) {
                            continue;
                        }
                        String left = loc(row.opt("left"), locale);
                        if (!left.isEmpty()) {
                            rows.add(new String[] { left, loc(row.opt("right"), locale) });
                        }
                    }
                    if (!rows.isEmpty()) {
                        out.add(new Block(type, label, "", rows, 0, 0));
                    }
                    break;
                }
                case "progress": {
                    double max = b.optDouble("max", 0);
                    if (max > 0) {
                        out.add(new Block(type, label, "", null, Math.max(0, b.optDouble("value", 0)), max));
                    }
                    break;
                }
                case "divider":
                    out.add(new Block(type, "", "", null, 0, 0));
                    break;
                default:
                    break;  // a type from a newer file: skipped
            }
        }
        return out;
    }

    private static Popup popup(JSONObject p, Locale locale) {
        if (p == null) {
            return null;
        }
        String id = p.optString("id", "").trim();
        String title = loc(p.opt("title"), locale);
        List<Block> body = blocks(p.optJSONArray("blocks"), locale);
        if (id.isEmpty() || (title.isEmpty() && body.isEmpty())) {
            return null;
        }
        List<String[]> buttons = new ArrayList<>();
        JSONArray b = p.optJSONArray("buttons");
        for (int i = 0; b != null && i < b.length(); i++) {
            JSONObject button = b.optJSONObject(i);
            if (button == null) {
                continue;
            }
            String label = loc(button.opt("label"), locale);
            String url = button.optString("url", "").trim();
            String action = button.optString("action", "").trim();
            if (label.isEmpty()) {
                continue;
            }
            if (url.startsWith("https://")) {
                buttons.add(new String[] { label, url });
            } else if (action.equals("support")) {
                buttons.add(new String[] { label, "support" });
            }
        }
        return new Popup(id, title, body, buttons);
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
