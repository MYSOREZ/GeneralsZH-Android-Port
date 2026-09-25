# Translation queue (max 20 concurrent agents)
Running: ru review A-D, uk review A-D, de parts A-D, fr parts A-D, web-verify glossary de/fr/es/br
Next, in order:
1. web-verify glossary: polish, chinese, korean, interslavic, arabic, persian
2. parts A-D (000-008, 009-016, 017-024, 025-032) after that language's web-verify is done:
   spanish, brazilian, polish, chinese, korean, interslavic, arabic, persian
   -> group D also translates 033 (legacy labels)
3. 033 (legacy labels) for german, french, ukrainian (one agent)
4. de/fr: apply glossary_changes.json to out/*.json (term replacement, then re-check)
5. assemble every language: translate_kit.py assemble chunks/ <lang>/out/ ; russian drops CREDITS:SSDevTeam1-3
6. ar/fa: keep out of languages/ on main until the engine renders RTL (user decides)
