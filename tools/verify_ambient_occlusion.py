"""Native pixel contract for ambient occlusion: indirect light only, and a depth
pre-pass that leaves the image as it was.

python tools/verify_ambient_occlusion.py --build build-rel (Pillow required).

A floor meets a wall; two boxes stand on the floor, one whose material has
`ao` 0.2, and an alpha-cut card stands in front. Each lighting is captured
with ambient occlusion on and off:

- under direct light alone the two captures are identical to the bit: the
  depth pre-pass (on only with AO) leaves no hole, no z-fighting and no cut-out
  of its own, and neither the screen-space nor the material occlusion reaches
  direct light -- the two boxes' tops are equally lit;
- under ambient light alone AO only darkens, somewhere, and leaves the open
  floor exactly as it was; the material's `ao` darkens its box.
"""
import argparse
import base64
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat

WIDTH, HEIGHT = 640, 360
CAMERA = (0.0, 1.6, 4.0)
TARGET = (0.0, 0.7, -0.5)
FOV_DEGREES = 60.0


def card_model():
    """A 1 m quad facing +z whose texture is a checkerboard of opaque and cut cells."""
    payload = struct.pack('<12f', -.5,-.5,0, .5,-.5,0, .5,.5,0, -.5,.5,0)
    payload += struct.pack('<12f', *([0,0,1]*4))
    payload += struct.pack('<8f', 0,1, 1,1, 1,0, 0,0)
    payload += struct.pack('<6H', 0,1,2,0,2,3)
    checker = Image.new('RGBA', (64, 64), (128, 128, 128, 0))
    for y in range(0, 64, 16):
        for x in range(0, 64, 16):
            if (x // 16 + y // 16) % 2 == 0:
                checker.paste((128, 128, 128, 255), (x, y, x + 16, y + 16))
    image = io.BytesIO(); checker.save(image, format='PNG')
    data = payload + image.getvalue()
    return {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'buffers': [{'byteLength': len(data),
                     'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()}],
        'bufferViews': [{'buffer': 0, 'byteOffset': o, 'byteLength': n}
                        for o, n in ((0, 48), (48, 48), (96, 32), (128, 12), (len(payload), len(image.getvalue())))],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3',
             'min': [-.5, -.5, 0], 'max': [.5, .5, 0]},
            {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC3'},
            {'bufferView': 2, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
            {'bufferView': 3, 'componentType': 5123, 'count': 6, 'type': 'SCALAR'}],
        'materials': [{'doubleSided': True, 'alphaMode': 'MASK', 'alphaCutoff': .5,
                       'pbrMetallicRoughness': {'baseColorTexture': {'index': 0},
                                                'metallicFactor': 0, 'roughnessFactor': 1}}],
        'images': [{'bufferView': 4, 'mimeType': 'image/png'}], 'textures': [{'source': 0}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
                                    'indices': 3, 'material': 0}]}],
        'nodes': [{'mesh': 0}]}


def box(name, position, scale, ao=1.0):
    return {'type': 'MeshNode', 'name': name, 'mesh': 'cube', 'baseColor': [.5, .5, .5, 1],
            'metallic': 0, 'roughness': 1, 'ao': ao, 'castShadows': True,
            'transform': {'position': list(position), 'scale': list(scale)}}


def project(point):
    """The pixel a world point lands on, through the capture's camera."""
    forward = [t - c for t, c in zip(TARGET, CAMERA)]
    norm = math.sqrt(sum(f * f for f in forward)); forward = [f / norm for f in forward]
    right = [-forward[2], 0.0, forward[0]]  # forward x world up
    norm = math.sqrt(sum(r * r for r in right)); right = [r / norm for r in right]
    up = [right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
          right[0] * forward[1] - right[1] * forward[0]]
    d = [p - c for p, c in zip(point, CAMERA)]
    x, y, z = (sum(a * b for a, b in zip(d, axis)) for axis in (right, up, forward))
    focal = (HEIGHT / 2) / math.tan(math.radians(FOV_DEGREES) / 2)
    return WIDTH / 2 + focal * x / z, HEIGHT / 2 - focal * y / z


def region(point, half=4):
    x, y = project(point)
    return (round(x) - half, round(y) - half, round(x) + half, round(y) + half)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-rel'))
    build = parser.parse_args().build.resolve()
    runs = build / 'ambient-occlusion-run'
    runs.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='run-', dir=runs))
    project_dir = root / 'source'
    for folder in ('scenes', 'assets/models'):
        (project_dir / folder).mkdir(parents=True, exist_ok=True)
    (project_dir / 'assets/models/card.gltf').write_text(json.dumps(card_model()))

    settings = {'changeRenderingAtLoad': True, 'giEnabled': False, 'iblEnabled': False,
                'bloomEnabled': False, 'fogEnabled': False, 'clearColor': [0, 0, 0, 1],
                'aoRadius': .75, 'aoIntensity': 1, 'aoPower': 1.35}
    sun = {'type': 'LightNode', 'name': 'Sun', 'lightType': 0, 'direction': [-.35, -.8, -.5],
           'color': [1, 1, 1], 'intensity': 2, 'castShadows': True}
    card = {'type': 'Node', 'name': 'Card', 'importedFrom': 'assets/models/card.gltf',
            'transform': {'position': [-1.6, .6, .6], 'scale': [1, 1, 1]}}
    children = [box('Floor', (0, -.1, 0), (8, .2, 8)),
                box('Wall', (0, 1.5, -1.6), (8, 3, .2)),
                box('Box', (.9, .3, 0), (.6, .6, .6)),
                box('Occluded box', (-.3, .3, .2), (.6, .6, .6), ao=.2),
                card, sun]
    scene = {'schema': 2, 'version': 2, 'scene': {'type': 'Scene', 'name': 'AO', 'children': children,
                                                  'settings': settings}}
    proj = project_dir / 'AO.saidaproj'
    proj.write_text(json.dumps({'schema': 1, 'version': 1, 'name': 'AO',
                                'engineVersion': '1.0.0', 'mainScene': 'scenes/main.scene'}))
    env = dict(os.environ, SAIDA_WINDOW_HIDDEN='1', SAIDA_WINDOW_SIZE=f'{WIDTH}x{HEIGHT}')
    bundle = root / 'player'
    captures = {}
    for lighting in ('direct', 'ambient'):
        for ao in (True, False):
            name = f'{lighting}-{"ao" if ao else "noao"}'
            settings.update(aoEnabled=ao, ambient=[0] * 3 if lighting == 'direct' else [.5] * 3)
            sun['intensity'] = 2 if lighting == 'direct' else 0
            (project_dir / 'scenes/main.scene').write_text(json.dumps(scene))
            if not bundle.exists():
                subprocess.run([str(build / 'bin/saida_tool.exe'), 'export-game', str(proj),
                                '--platform', 'windows', '--out', str(bundle)], env=env,
                               check=True, stdout=subprocess.DEVNULL)
            else:
                (bundle / 'scenes/main.scene').write_bytes((project_dir / 'scenes/main.scene').read_bytes())
            capture = root / (name + '.png')
            with (root / (name + '.log')).open('w') as log:
                subprocess.run([str(bundle / 'AO.exe'), '--screenshot', str(capture), '--after-frames', '30',
                                '--camera-pos', ','.join(map(str, CAMERA)),
                                '--camera-look', ','.join(map(str, TARGET)),
                                '--camera-fov', str(FOV_DEGREES)],
                               env=env, check=True, stdout=log, stderr=log, timeout=90)
            with Image.open(capture) as im:
                captures[name] = im.convert('RGB')

    def mean(image, box_region):
        return sum(ImageStat.Stat(image.crop(box_region)).mean) / 3

    lit, unlit = captures['direct-ao'], captures['direct-noao']
    difference = ImageChops.difference(lit, unlit).getbbox()
    assert difference is None, f'direct light changed with ambient occlusion on, within {difference}'
    tops = (mean(lit, region((.9, .6, 0))), mean(lit, region((-.3, .6, .2))))
    assert min(tops) > 40 and abs(tops[0] - tops[1]) <= 1, f'material ao reached direct light: {tops}'

    occluded, open_ = captures['ambient-ao'], captures['ambient-noao']
    darker = ImageChops.subtract(open_, occluded)   # positive where AO darkened
    brighter = ImageChops.subtract(occluded, open_)
    assert brighter.getbbox() is None, f'ambient occlusion brightened pixels within {brighter.getbbox()}'
    darkened = sum(darker.convert('L').histogram()[3:])
    assert darkened > 500, f'ambient occlusion darkened only {darkened} pixels'
    floor = region((0, 0, 2.6), 6)
    assert ImageChops.difference(occluded.crop(floor), open_.crop(floor)).getbbox() is None, \
        'the open floor, far from everything, changed with ambient occlusion'
    crease = mean(occluded, region((.2, .05, -1.45), 3)), mean(open_, region((.2, .05, -1.45), 3))
    assert crease[0] < crease[1] - 3, f'the floor-wall crease is not occluded: {crease}'
    ambient_tops = (mean(open_, region((.9, .6, 0))), mean(open_, region((-.3, .6, .2))))
    assert ambient_tops[1] < ambient_tops[0] * .4, f'material ao does not darken ambient light: {ambient_tops}'

    print('PASS ambient occlusion: direct light identical with AO on and off;',
          f'{darkened} pixels darkened under ambient light, open floor unchanged;',
          json.dumps({'direct-tops': tops, 'ambient-tops': ambient_tops, 'crease': crease}))
    print('Captures:', root)


if __name__ == '__main__':
    main()
