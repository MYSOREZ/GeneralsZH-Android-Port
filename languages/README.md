# Language packs

A language pack is one plain-text file: `languages/<language>/generals.str`.

That is the whole format. UTF-8, one entry per label, editable in any editor, and
reviewable as a normal diff in a pull request. Translating the game means editing
strings in this directory and opening a PR; there is nothing to compile and no
binary to produce.

```
GUI:GameOptions
"ОПЦИИ ИГРЫ"
END
```

A label line, the translated string in double quotes, then `END`. Lines starting
with `//` are comments. Escapes are `\n`, `\t`, `\"` and `\\`. Leave the label
lines exactly as they are — the game looks strings up by label, so a changed
label is a string the game can no longer find.

## Labels the port adds

A few labels exist in no original `generals.csf`, because the GeneralsX port added the
things they name: the Steam release's "Custom Mission" button (`GUI:CustomMission`) and the
touch force-attack button (`GX:ForceAttack`, `GX:ToolTipForceAttack`). They are translated
in the pack like everything else -- see the end of `russian/generals.str`. Their English
text is built into the engine, so a game without a pack, or a pack that does not have them
yet, shows English there rather than `MISSING`. Still one file per language.

## Why a .str and not a .csf

The engine has always read both: a compiled binary `.csf`, and this plain-text
`.str`, which was the development format. `GameTextManager::init()` prefers the
`.str` when one exists.

Until now the text format could only hold Latin-1 — every byte became the code
point with the same value — so a Russian or Greek translation could not be
written in it at all, and had to be shipped as a compiled `.csf` instead. That is
why every community translation is a `.big` archive that overwrites
`Data\English`: not because anyone wanted it that way, but because the reviewable
format could not carry the alphabet. `.str` files are now decoded as UTF-8
(`GameTextManager::translateCopy`), and the path the engine looks in is
per-language (`data/<language>/generals.str`), so any language can be a text file
again.

## Where it goes on a device

The engine asks for `data/<language>/generals.str` inside the game folder, with
the language being what the launcher's language setting maps to — `russian`,
`german`, `spanish`, `french`, `korean`, `polish`, `brazilian`, `chinese`. Copy
the file there and set the launcher's language; nothing else is needed.

The launcher's Diagnostics section will fetch these packs directly in a later
version. The format is settled first so that translations started now stay
valid.

## Converting an existing translation

A `.csf` cannot be renamed into a `.str` -- it is binary. Its text is UTF-16LE with
every byte bitwise inverted, wrapped in a table of contents. `scripts/language/csf2str.py`
undoes that and writes the UTF-8 text file:

```
python3 scripts/language/csf2str.py Generals.csf -o languages/<language>/generals.str
```

It also reads a `.big` archive directly, since community translations ship as one,
and takes the first `generals.csf` inside whatever language folder the archive
happens to file it under:

```
python3 scripts/language/csf2str.py 00RussianZH.big -o languages/russian/generals.str
```

Nothing else is needed: the output is the pack.

## Russian

`languages/russian/generals.str` started as the community `00RussianZH.big` translation
(3991 labels, decompiled back to text). On 25/09/2026 it was reviewed line by line against
the English original of the current game data (`EnglishZH.big`): the 2458 labels it lacked
(nearly all mission subtitles) were translated, lines still in English were translated,
lines that said something the English does not (the "ЛОКАЛИЗАЦИЯ 2003 SIBERIAN STUDIO"
on the loading screen, the translator's own `CREDITS:SSDevTeam1-3`) were brought back to
the English, and good existing lines were kept as they were. 6447 labels now: the full
English set plus 22 legacy labels from older game data (`GUI:GroupRoom15-22`,
`GUI:BuddyAddReqMessage*`, the misspelled `GUI:CÀontrolBarBack`, ...), kept so the pack
works with both. Work files: `tools/translation-work/`.
