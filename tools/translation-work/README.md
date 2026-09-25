# Game-text translation, work in progress

Not a language pack: the launcher only reads `languages/<language>/generals.str`.
This is the state of the translation into the launcher's languages, kept so the work
survives between sessions. Tooling: `scripts/language/translate_kit.py`.

- `english.str` -- source, from EnglishZH.big (`csf2str.py`) plus the three port labels.
- `chunks/NNN.json` -- the source in 200-entry chunks; `033.json` holds 22 legacy labels
  (older game versions) kept so the packs work with both old and new game data.
- `<language>/glossary.json` -- unit/building/upgrade/power names for that language.
- `<language>/out/NNN.json` -- translated chunks; each passes `translate_kit.py check`.
- `QUEUE.md` -- what is left.

Russian and Ukrainian are reviews of the existing packs against the English (keep good
lines, translate missing ones, drop translator/studio insertions); the review inputs are
regenerated from `languages/<language>/generals.str` plus `chunks/`.
