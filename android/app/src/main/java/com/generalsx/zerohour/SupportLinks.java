package com.generalsx.zerohour;

import android.content.Context;

import java.util.ArrayList;
import java.util.List;

// GeneralsX @feature Android port 03/10/2026 Where a donation can be sent, as the launcher's Help
// page lists it (README "Support the project").
//
// The list comes from the signed settings (update/config.json on the updates branch, see
// docs/HOWTO/PUBLISH_UPDATE.md), so an address can be added, changed or retired without a new APK:
//
//   "support_1": "USDT — TRON (TRC20)|TAQHCF733ovKpvBjUgvkE6wHxkntnKZ6br",
//   "support_2": "Boosty|https://boosty.to/...",
//
// one entry per key, label and value split by the first '|', numbered from 1 without gaps (the list
// ends at the first missing number). A value starting with https:// is a link and opens in the
// browser; anything else is an address and is copied. The settings are signed, which matters more
// here than anywhere else: an address swapped in transit would send the money to someone else, and
// a forged manifest is rejected before any of it reaches remote_config.ini.
//
// Until the settings name any support_N entry, the list built into this APK is shown -- the same
// three addresses as the README. Once they name one, they replace the built-in list entirely, so
// an address can also be withdrawn; "support_1": "none" (no '|') withdraws them all, and the card
// is then not shown. The built-in list is only for a launcher that has never fetched settings:
// once an address dies it must not come back from an old APK's copy.
final class SupportLinks {

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

    private static final String[][] BUILT_IN = {
        { "USDT — TRON (TRC20)", "TAQHCF733ovKpvBjUgvkE6wHxkntnKZ6br" },
        { "USDT — BSC (BEP20)", "0x52c05c81485d68367385ff389cf19a453f036310" },
        { "USDT — TON", "UQAOdBpFSPhlgbvUIJ2O2w2NuwWashaNjFWDsOirDqH9kGbR" },
    };

    private static final int MAX_ENTRIES = 32;

    private SupportLinks() {
    }

    static List<Entry> load(Context ctx) {
        List<Entry> remote = new ArrayList<>();
        boolean published = false;
        for (int i = 1; i <= MAX_ENTRIES; i++) {
            String raw = UpdateManager.remoteConfig(ctx, "support_" + i, null);
            if (raw == null) {
                break;
            }
            published = true;
            Entry e = parse(raw);
            if (e != null) {
                remote.add(e);
            }
        }
        if (published) {
            return remote;
        }
        List<Entry> builtIn = new ArrayList<>();
        for (String[] e : BUILT_IN) {
            builtIn.add(new Entry(e[0], e[1]));
        }
        return builtIn;
    }

    // "label|value"; an entry without a label, without a value, or with a link that is not https
    // is skipped rather than shown half-formed.
    private static Entry parse(String raw) {
        int bar = raw.indexOf('|');
        if (bar <= 0) {
            return null;
        }
        String label = raw.substring(0, bar).trim();
        String value = raw.substring(bar + 1).trim();
        if (label.isEmpty() || value.isEmpty() || value.indexOf(' ') >= 0) {
            return null;
        }
        if (value.contains("://") && !value.startsWith("https://")) {
            return null;
        }
        return new Entry(label, value);
    }
}
