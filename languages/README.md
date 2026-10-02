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
touch force-attack button (`GX:ForceAttack`, `GX:ToolTipForceAttack`), and the page arrows
on a builder's command bar (`GX:BuilderPageMore`, `GX:ToolTipBuilderPageMore`,
`GX:BuilderPageBack`, `GX:ToolTipBuilderPageBack`), and two GeneralsOnline buttons
(`GX:ViewMatchOnline` on the score screen, `GX:Logout` in the player info). They are translated
in the pack like everything else -- see the end of `russian/generals.str`. Their English
text is built into the engine, so a game without a pack, or a pack that does not have them
yet, shows English there rather than `MISSING`. Still one file per language.

## The language's own name

Every pack starts with a `GX:LanguageName` entry holding the language's name in that language
("Русский", "العربية", "فارسی"). The launcher shows that name in its language picker, so a new
pack needs nothing more than its folder: `languages/<folder>/generals.str` with this entry at the
top. A pack without it is listed under its folder name with a capital letter.

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
with a few picks by the repository owner: ГЛА (not "МАО", and in Cyrillic like США), Хеликс, Крестоносец, Техничка,
Залповая установка Скад (the SCUD Launcher stays "Эльбрус").

## Ukrainian

`languages/ukrainian/generals.str` started as the community `00_UA_ZeroHour.big` translation.
On 25/09/2026 it was checked against the same English original: lines left in English and
the mission subtitles it was missing (campaign dialogue, general taunts, unit descriptions)
were translated with the pack's own unit names, and the 22 legacy labels were added, so it
has the same 6447 labels as the Russian pack. The faction is "ГЛА" throughout (the pack had
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

## Simplified Chinese

`languages/chinese/generals.str` is a new translation from the English original into Simplified
Chinese. It uses the names the Chinese-speaking community uses for the game: "GLA", "指挥中心",
"战车工厂", "飞毛腿风暴", "粒子加农炮", "黑莲花", "霸王坦克" for the Overlord and "皇帝坦克" for the
Emperor, "萨拉克斯博士" for Dr. Thrax, and so on. Hotkeys follow the Chinese convention of a
Latin letter in brackets after the name ("推土机(&D)"), so `&`+letter counts still match the
English. Tooltips read "强于：… / 弱于：…", "所需电力：", "冷却时间：", "召唤地点：". Multiplayer map
names stay in English. Jokes are localised ("小兔子乖乖，把门儿开开" for the test rhyme, "滴滴香浓，
意犹未尽" for "Good to the last drop", "一号货架有东西洒了！").

The text needs a font with CJK glyphs. The APK only bundles Latin/Cyrillic fonts, so the
engine takes any glyph the game font lacks from a fallback face: first `fonts/fallback.ttf`
(or `.otf`/`.ttc`) in the game-data folder if the player put one there, then the system CJK font
from `/system/fonts` (Noto Sans CJK on current Android, Droid Sans Fallback on old devices), or
on desktop Linux the "Noto Sans CJK SC"/"WenQuanYi Zen Hei" families via fontconfig. A device
with none of these shows empty boxes; copying any CJK `.ttf` to `fonts/fallback.ttf` fixes it.

## Korean

`languages/korean/generals.str` is a new translation from the English original into Korean. It
uses the names of the Korean release: "사령부", "군수 공장", "스커드 스톰", "입자 캐논",
"블랙 로투스", "오버로드", "황제" for the Emperor, "파괴 공작원" for the Saboteur, "스랙스 박사" for
Dr. Thrax, and so on. EVA and the officers speak the formal 합쇼체 and call the player "장군님";
the enemy generals speak down to the player ("장군", 반말), and the USA boss's "Boy/Kid/Punk"
become "꼬마" and "애송이". Hotkeys follow "이름(&K)". Tooltips read "강함: … / 약함: …",
"필요 전력:", "재사용 대기시간:", "사용 위치:". Multiplayer map names stay in English. Jokes are
localised ("산토끼 토끼야" for the test rhyme, "소리 없이 강하다" for "Silent but deadly").

Like the Chinese pack, it takes its Hangul glyphs from the fallback face; see the note under
Simplified Chinese.


## Arabic

`languages/arabic/generals.str` is a new translation from the English original into Modern
Standard Arabic, with a colloquial touch where a joke needs one. The GLA is "جيش التحرير
العالمي", and unit names follow the usual Arabic renderings: "مركز القيادة", "مصنع الحرب",
"عاصفة سكود", "مدفع الجسيمات", "اللوتس الأسود", "أوفرلورد", and "الإمبراطور" for the Emperor.
The USA boss's "Boy/Kid/Punk" become "يا ولد" and "يا صعلوك". Hotkeys follow "الاسم (&K)".
Tooltips read "قوي ضد: … / ضعيف ضد: …", "الطاقة المطلوبة:", "مؤقت العد التنازلي:" and
"يُطلق من:". Multiplayer map names stay in English. Jokes are localised: "ماما زمانها جاية"
for the test rhyme, "انسكاب في الممر رقم واحد!", "على قد لحافك مد رجليك" for "you don't
start the dance if you can't pay the bill", and "ما أطيب رائحة أم القنابل في الصباح الباكر".

The text is stored as ordinary Arabic, in logical order and with base letters. The engine
shapes it into joined forms and lays it out right to left (`render2dsentence.cpp`, see the
diary entry of 27/09/2026). Harakat such as shadda and tanween stay in the file, but the engine
drops them when drawing, because the glyph-per-cell layout cannot stack a mark on its letter.
The glyphs come from the system Arabic font (Noto Naskh Arabic on Android), which covers every
presentation form the shaper produces.

## Persian

`languages/persian/generals.str` is a new translation from the English original into Persian.
The GLA stays "GLA", and unit names follow the renderings Persian players know: "مرکز فرماندهی",
"کارخانه جنگ", "طوفان اسکاد", "توپ ذره‌ای", "نیلوفر سیاه", "اورلورد" and "امپراتور". EVA is
"ایوا". Generals keep their names in transliteration ("ژنرال لیانگ", "دکتر تراکس", "شاهزاده
قصاد"). Hotkeys follow "نام (&K)". Tooltips read "قوی در برابر: … / ضعیف در برابر: …",
"برق مورد نیاز:", "زمان شمارش معکوس:" and "استقرار از:". Static numbers use Persian digits;
numbers that the game fills in at runtime stay as the engine prints them. Multiplayer map names
stay in English.

Characters speak in their own register: the American boss talks colloquial Tehrani ("پسر",
"بچه‌جون", "جوجه"), the others speak standard written Persian. Jokes are localised: "اتل متل
توتوله" for the test rhyme, "مدرکم را از جلوی دانشگاه، میدان انقلاب، می‌خریدم" for Dr. Thrax's
mail-order degree, "وقتی پول مطرب رو نداری مجلس رقص راه ننداز" for "you don't start the dance if
you can't pay the bill", "بی‌صدا ولی بودار", "ظرفیت سم ندارند" and "مدیرکل بهداشت" for the
Surgeon General jab.

Persian spelling needs the zero-width non-joiner (U+200C) inside words such as "می‌کند". The
file keeps it; the engine's shaper uses it to stop the join and then drops it, so it takes no
cell. The Persian letters (پ چ ژ گ ک ی) shape into the FB50 presentation forms, all of which are
in Noto Naskh Arabic. Everything else about right-to-left layout is as described for Arabic.
