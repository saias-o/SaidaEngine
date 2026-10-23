"""GPU regression: contact shadows, grazing light, range and camera travel.

Run with python tools/verify_shadows.py --build build-rel (Pillow required).
GI, AO and IBL are disabled so only the actual direct-light shadow is measured.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageStat

WIDTH, HEIGHT = 800, 600


def box(name, position, scale):
    return {'type': 'MeshNode', 'name': name, 'mesh': 'cube',
            'baseColor': [.5, .5, .5, 1], 'metallic': 0, 'roughness': 1,
            'castShadows': True,
            'transform': {'position': position, 'scale': scale}}


def pixel(point, eye, target):
    forward = [t-c for t, c in zip(target, eye)]
    norm = math.sqrt(sum(v*v for v in forward))
    forward = [v/norm for v in forward]
    right = [-forward[2], 0, forward[0]]
    norm = math.sqrt(sum(v*v for v in right))
    right = [v/norm for v in right]
    up = [right[1]*forward[2]-right[2]*forward[1],
          right[2]*forward[0]-right[0]*forward[2],
          right[0]*forward[1]-right[1]*forward[0]]
    delta = [v-c for v, c in zip(point, eye)]
    x, y, z = [sum(v*a for v, a in zip(delta, axis)) for axis in (right, up, forward)]
    focal = HEIGHT/2/math.tan(math.radians(52)/2)
    return round(WIDTH/2+focal*x/z), round(HEIGHT/2-focal*y/z)


def mean(image, point, eye, target):
    x, y = pixel(point, eye, target)
    return sum(ImageStat.Stat(image.crop((x-2, y-2, x+3, y+3))).mean)/3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-rel'))
    parser.add_argument('--label', default='current')
    args = parser.parse_args()
    build = args.build.resolve()
    runs = build/'shadow-run'
    runs.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=args.label+'-', dir=runs))
    source = root/'source'
    (source/'scenes').mkdir(parents=True, exist_ok=True)
    proj = source/'Shadow.saidaproj'
    env = dict(os.environ, SAIDA_WINDOW_HIDDEN='1', SAIDA_WINDOW_SIZE=f'{WIDTH}x{HEIGHT}')
    bundle = root/'player'
    results = []
    cases = [(*case, 1.0) for case in ((.15, 60, 0, 1, False), (.5, 25, 0, 1, False),
             (.15, 120, 0, 1, False), (.5, 25, 150, 1, False),
             (.15, 60, -150, 1, False), (.15, 60, 0, -1, False),
             (4, 25, 0, 1, False), (.5, 25, 0, 1, True))]
    cases += [(.15, 60, 0, 1, False, 1.5), (.15, 60, 0, 1, False, 0)]
    for elevation, distance, shift, sign, spot, softness in cases:
        name = f'{"spot" if spot else "sun"}-{elevation}-range-{distance}-shift-{shift}-dir-{sign}-soft-{softness}'
        eye, target = [shift, 4, 8], [shift, 0, 0]
        floor = box('Floor', [shift, -.1, 0], [30, .2, 20])
        caster = box('Contact', [shift, .9, 0], [.24, 1.8, .4])
        sun = {'type': 'LightNode', 'lightType': 2 if spot else 0,
               'direction': [sign, -elevation, 0],
               'intensity': 3, 'castShadows': True}
        if spot:
            sun.update(transform={'position': [shift-4, 2, 0]}, range=20,
                       spotInnerAngle=35, spotOuterAngle=45)
        settings = {'changeRenderingAtLoad': True, 'giEnabled': False, 'aoEnabled': False,
                    'iblEnabled': False, 'bloomEnabled': False, 'fogEnabled': False,
                    'ambient': [.015]*3, 'clearColor': [0, 0, 0, 1]}
        scene = {'schema': 2, 'version': 2, 'scene': {'type': 'Scene', 'name': 'Shadow',
                 'children': [floor, caster, sun], 'settings': settings}}
        proj.write_text(json.dumps({'schema': 1, 'version': 1, 'name': 'Shadow',
                        'engineVersion': '1.0.0', 'mainScene': 'scenes/main.scene',
                        'rendering': {'shadowDistance': distance, 'shadowResolution': 2048,
                                      'shadowSoftness': softness}}))
        images = []
        for enabled in (True, False):
            sun['castShadows'] = enabled
            (source/'scenes/main.scene').write_text(json.dumps(scene))
            if not bundle.exists():
                subprocess.run([str(build/'bin/saida_tool.exe'), 'export-game', str(proj),
                                '--platform', 'windows', '--out', str(bundle)],
                               env=env, check=True, stdout=subprocess.DEVNULL)
            else:
                (bundle/'scenes/main.scene').write_text(json.dumps(scene))
                # The player reads rendering settings from its exported project.
                for exported in bundle.glob('*.saidaproj'):
                    exported.write_text(proj.read_text())
            image = root/f'{name}-{"on" if enabled else "off"}.png'
            with (image.with_suffix('.log')).open('w') as log:
                subprocess.run([str(bundle/'Shadow.exe'), '--screenshot', str(image),
                                '--after-frames', '8', '--camera-pos', ','.join(map(str, eye)),
                                '--camera-look', ','.join(map(str, target)), '--camera-fov', '52'],
                               env=env, check=True, stdout=log, stderr=log, timeout=90)
            with Image.open(image) as im:
                images.append(im.convert('RGB'))
        contact = [shift+sign*.28, .001, 0]
        open_points = ([shift+x, .001, z] for x, z in ((-2, 1), (2, 2), (-2, -2)))
        contact_ratio = mean(images[0], contact, eye, target)/max(mean(images[1], contact, eye, target), 1)
        open_error = max(abs(mean(images[0], p, eye, target)-mean(images[1], p, eye, target)) for p in open_points)
        result = {'case': name, 'contact_ratio': contact_ratio, 'open_error': open_error,
                  'pass': contact_ratio < .65 and open_error < 2}
        results.append(result)
        print(json.dumps(result), flush=True)
    (root/'report.json').write_text(json.dumps(results, indent=2))
    assert all(r['pass'] for r in results), f'shadow regression failed: {root}'
    print(f'PASS contact shadows and unoccluded floor: {root}')


if __name__ == '__main__':
    main()
