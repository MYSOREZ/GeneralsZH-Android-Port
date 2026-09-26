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

Unit and faction names follow the original community translation, never player slang,
with a few picks by the repository owner: GLA (not "МАО"), Хеликс, Крестоносец, Техничка,
Залповая установка Скад (the SCUD Launcher stays "Эльбрус").

## Ukrainian

`languages/ukrainian/generals.str` started as the community `00_UA_ZeroHour.big` translation.
On 25/09/2026 it was checked against the same English original: lines left in English and
the mission subtitles it was missing (campaign dialogue, general taunts, unit descriptions)
were translated with the pack's own unit names, and the 22 legacy labels were added, so it
has the same 6447 labels as the Russian pack. The faction is "GLA" throughout (the pack had
"ГВА" in some places), matching the Russian pack.

## German

`languages/german/generals.str` is a new translation from the English original. It uses the
names of the official German release rather than the English ones: the faction is "GBA",
units and buildings are "Kommandozentrale", "Vierlingskanone", "Horchposten", "Tarnkappenjäger"
and so on. Multiplayer map names stay in English. Jokes, ad parodies and idioms in the
generals' taunts are carried over as German ones rather than word for word.

## French

`languages/french/generals.str` is a new translation from the English original. It uses the
names of the official French release: "GLA", "Centre de commandement", "Usine d'armement",
"Canon quadruple", "Poste d'écoute", "Tempête de SCUD", "Pirate de la route" and so on. French
typography keeps a space before `!`, `?` and `:`; EVA and officers address the player with
"vous". Multiplayer map names stay in English, apart from the "Tournament" maps. Jokes and
film or ad parodies in the generals' taunts are carried over as French ones (the Ghostbusters
"effluves", "J'adore l'odeur de la MOAB au petit matin") rather than word for word.

## Interslavic

`languages/interslavic/generals.str` is a new translation from the English original into
Interslavic (Medžuslovjansky), written in the standard Latin orthography (`ě č š ž`, no
etymological letters such as `ę`, `ć`, `ń`), so it reads the same to speakers of any Slavic
language. There is no official release to borrow names from: the faction stays "GLA", named
units keep their English names (Crusader, Scorpion, Comanche, Chinook, Helix, Raptor, Aurora,
Battlemaster, Overlord), and generic ones are translated ("Komandny centr", "Česticovo dělo",
"Burja SCUD", "Podslušny post", "Minna pastka"). EVA and officers address the player as "vy"
and "generale". Unlike the German and French packs, multiplayer map names are translated, as
in the Russian and Ukrainian ones. The generals' taunts keep their jokes and register (the
USA general's "momče"/"sinko", the MOAB "in the morning" line) rather than going word for word.

## Spanish

`languages/spanish/generals.str` is a new translation from the English original. It uses the
names of the official Spanish (Spain) release: the GLA is "ELG", the USA "EE. UU." and
the PLA "EPL", and units and buildings are "Centro de mando", "Tormenta SCUD", "Cañón de
partículas", "Red de túneles", "Cañón cuádruple", "Secuestrador", "Loto Negro" and so on. The
interface talks to the player with "tú"; EVA, officers and the enemy generals use "usted" and
"general", apart from the USA boss, who calls the player "chaval" and "mocoso" as in English.
Multiplayer map names stay in English. Jokes and film or ad parodies are carried over as
Spanish ones ("Otro que muerde el polvo", "Me encanta el olor a MOAB por la mañana", "¡Limpieza
en el pasillo uno!") rather than word for word.

## Brazilian Portuguese

`languages/brazilian/generals.str` is a new translation from the English original into
Brazilian Portuguese. It uses the names of the official Brazilian release: "GLA" (feminine, "a
GLA"), "EUA", "Centro de Comando", "Tempestade SCUD", "Canhão de Partículas", "Lótus Negra",
"Escavadeira de Construção", "Imperador" for the Emperor Overlord, and so on. Everyone speaks
to the player with "você"; EVA and the officers add "senhor" and "General", and the USA boss
calls the player "moleque" and "garoto" as in English. Multiplayer map names stay in English.
Jokes are carried over as Brazilian ones ("Atirei o pau no gato" for the test rhyme, "Adoro o
cheiro de MOAB pela manhã", "Limpeza no corredor um!", "toca a boiaaada!").

## Polish

`languages/polish/generals.str` is a new translation from the English original into Polish. It
uses the names of the Polish release where they exist: "GLA", "ALW" for the PLA, "Centrum
dowodzenia", "Burza SCUD", "Działo cząsteczkowe", "Czarny Lotos", "Spycharka", "Fabryka broni",
"Emperor" for the Emperor Overlord, and so on. Officers address the player as "generale" and
everyone uses the 2nd person singular; the USA boss's "Boy/Kid/Punk" become "chłopcze", "mały"
and "gnojku". Tooltips follow one pattern ("Skuteczny przeciw: czołgom / Słaby przeciw:
samolotom", "Wymagana energia:", "Czas odnowienia:"). Multiplayer map names stay in English.
Jokes are carried over as Polish ones ("Wlazł kotek na płotek" for the test rhyme, "zapach MOAB
o poranku", "Rozlane w alejce pierwszej!").
