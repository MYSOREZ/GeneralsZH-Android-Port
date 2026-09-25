# usage: lang_rest.py <lang> <chunk>  -> prints lines not covered by the language glossary (by label)
import json,sys
lang,n=sys.argv[1],sys.argv[2]
g={e['label']:e['text'] for e in json.load(open(f'{lang}/glossary.json'))}
d=json.load(open(f'chunks/{n}.json'))
for i,x in enumerate(d):
    if x['label'] in g: continue
    print(f"{i}|{x['label']}|{x['text']}".replace('\n','\\n'))
