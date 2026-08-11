import json, os
from collections import Counter

MANIFEST = 'compliance/assets.json'
doc = json.load(open(MANIFEST, encoding='utf-8'))
assets = doc['assets']
by_path = {a['path']: a for a in assets}
existing = set(by_path)

KENNEY = "Kenney (kenney.nl) - CC0 1.0, no rights reserved"
NEW_KITS = {
    'city':   "Kenney City Kit (Commercial) 2.1, OBJ variant used as-is with its shared colormap.",
    'suburb': "Kenney City Kit (Suburban) 20, OBJ variant used as-is with its shared colormap.",
    'roads':  "Kenney City Kit (Roads), OBJ variant used as-is with its shared colormap.",
    'cars':   "Kenney Car Kit, OBJ variant used as-is with its shared colormap.",
    'boats':  "Kenney Pirate Kit 2.1, the two rowing boats only, used as-is with its shared colormap.",
}


def kind_for(path):
    if path.endswith('.png') or path.endswith('.hdr'):
        return 'image'
    if path.endswith('.ogg'):
        return 'audio'
    return 'model'


tracked = {e.lower() for e in doc['trackedExtensions']}

added = []
for root, _, files in os.walk('GTAClone/assets'):
    for f in sorted(files):
        path = os.path.join(root, f).replace(os.sep, '/')
        # The manifest covers distributable art, not the sidecar data files the
        # pipeline writes next to it.
        if os.path.splitext(f)[1].lower() not in tracked:
            continue
        if path in existing:
            continue
        kit = os.path.basename(root)
        if f.endswith('-body.obj') or f.endswith('-wheel.obj'):
            prov = ("Kenney Car Kit, split into chassis and wheel by "
                    "GTAClone/tools/split_car_wheels.py so a driven vehicle can "
                    "steer and spin them; CC0 permits the modification.")
            holder = KENNEY
        elif kit in NEW_KITS:
            prov, holder = NEW_KITS[kit], KENNEY
        else:
            # Copied from the vertical slice: carry its declaration across
            # verbatim rather than inventing a second provenance for the same
            # bytes.
            src = path.replace('GTAClone/', 'VerticalSlice/')
            if src not in by_path:
                raise SystemExit('undeclared source for %s' % path)
            prov, holder = by_path[src]['provenance'], by_path[src]['copyrightText']
        added.append({
            'path': path, 'kind': kind_for(path), 'distribution': True,
            'license': 'CC0-1.0', 'copyrightText': holder, 'provenance': prov,
        })

assets.extend(added)
assets.sort(key=lambda a: a['path'])
paths = [a['path'] for a in assets]
assert len(paths) == len(set(paths)), 'duplicate path in the manifest'
with open(MANIFEST, 'w', encoding='utf-8') as f:
    json.dump(doc, f, indent=2)
    f.write('\n')
print('added %d entries, %d total' % (len(added), len(assets)))
for src, n in Counter(a['provenance'].split(',')[0] for a in added).most_common():
    print('  %-42s %d' % (src, n))
