"""Native pixel contract for camera-facing atlases and coarse live lighting.

python tools/verify_billboard_materials.py --build build-rel (Pillow required).
"""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image, ImageStat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-rel'))
    build = parser.parse_args().build.resolve()
    runs = build / 'billboard-material-run'
    runs.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='run-', dir=runs))
    project = root / 'source'
    for folder in ('scenes', 'assets/models', 'assets/skies'):
        (project / folder).mkdir(parents=True, exist_ok=True)
    payload = struct.pack('<12f', -.75,-.75,0, .75,-.75,0, .75,.75,0, -.75,.75,0)
    payload += struct.pack('<12f', *([0,0,1]*4))
    payload += struct.pack('<8f', 0,1, 1,1, 1,0, 0,0)
    payload += struct.pack('<6H', 0,1,2,0,2,3)
    model = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'bufferViews': [{'buffer':0,'byteOffset':o,'byteLength':n}
                        for o,n in ((0,48),(48,48),(96,32),(128,12))],
        'accessors': [
            {'bufferView':0,'componentType':5126,'count':4,'type':'VEC3',
             'min':[-.75,-.75,0],'max':[.75,.75,0]},
            {'bufferView':1,'componentType':5126,'count':4,'type':'VEC3'},
            {'bufferView':2,'componentType':5126,'count':4,'type':'VEC2'},
            {'bufferView':3,'componentType':5123,'count':6,'type':'SCALAR'}],
        'materials':[{'doubleSided':True, 'alphaMode':'MASK', 'alphaCutoff':.35,
                      'pbrMetallicRoughness':{'baseColorTexture':{'index':0},
                                             'metallicFactor':0,'roughnessFactor':1},
                      'extras':{'SAIDA_billboard':{'columns':2,'rows':1}}}],
        'images':[{'bufferView':4,'mimeType':'image/png'}], 'textures':[{'source':0}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},
                                 'indices':3,'material':0}]}],
        'nodes':[{'mesh':0,'translation':[0,1,0]}]}
    settings = {'changeRenderingAtLoad':True,'giEnabled':False,'ambient':[.03]*3,
                'iblEnabled':False,'aoEnabled':False,'bloomEnabled':False,'fogEnabled':False}
    sun = {'type':'LightNode','name':'Sun','lightType':0,'direction':[0,-1,0],
           'color':[1,1,1],'intensity':2,'castShadows':False}
    scene = {'schema':2,'version':2,'scene':{'type':'Scene','name':'Billboard',
             'children':[{'type':'Node','name':'Panel','importedFrom':'assets/models/panel.gltf'},sun],
             'settings':settings}}
    proj = project / 'Billboard.saidaproj'
    proj.write_text(json.dumps({'schema':1,'version':1,'name':'Billboard',
                               'engineVersion':'1.0.0','mainScene':'scenes/main.scene'}))
    env = dict(os.environ, SAIDA_WINDOW_HIDDEN='1', SAIDA_WINDOW_SIZE='640x360')
    bundle = root / 'player'
    values = {}
    cases = {
        'top':([0,-1,0],2,[1,1,1],'0,1,5',False),
        'side':([1,0,0],2,[1,1,1],'0,1,5',False),
        'off':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'red':([0,-1,0],2,[1,0,0],'0,1,5',False),
        'rotated':([0,-1,0],2,[1,1,1],'5,1,0',False),
        'atlas-front':([0,-1,0],2,[1,1,1],'0,1,5',True),
        'atlas-side':([0,-1,0],2,[1,1,1],'5,1,0',True),
        'atlas-back':([0,-1,0],2,[1,1,1],'0,1,-5',True),
        'normal-up':([0,-1,0],2,[1,1,1],'0,1,5',False),
        'normal-up-side':([0,-1,0],2,[1,1,1],'5,1,0',False),
        'normal-away':([0,1,0],2,[1,1,1],'0,1,5',False),
        'normal-occluded':([0,-1,0],2,[1,1,1],'0,1,5',False),
        'normal-direct':([0,-1,0],2,[1,1,1],'0,1,5',False),
        'normal-ambient':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-ambient-occluded':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'mesh-ambient':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-sky-red':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-sky-blue':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-sky-blend':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-sky-occluded':([0,-1,0],0,[1,1,1],'0,1,5',False),
        'normal-coverage':([0,-1,0],2,[1,1,1],'5,1,0',False),
        'normal-local-x':([-1,0,0],2,[1,1,1],'0,1,5',False),
        'normal-yawed-x':([-1,0,0],2,[1,1,1],'0,1,5',False)}
    for name,(direction,intensity,color,camera,colored) in cases.items():
        model['materials'][0]['extras'] = {'SAIDA_billboard':{'columns':2,'rows':1}}
        if name == 'mesh-ambient':
            model['materials'][0].pop('extras')
        atlas = Image.new('RGBA',(32,16),(160,160,160,255))
        if colored:
            atlas.paste((160,0,0,255),(0,0,16,16))
            atlas.paste((0,160,0,255),(16,0,32,16))
        if name == 'normal-coverage':
            atlas.paste((160,160,160,0),(16,0,32,16))
        image = io.BytesIO(); atlas.save(image,format='PNG')
        data = payload + image.getvalue()
        model['bufferViews'] = model['bufferViews'][:4] + [
            {'buffer':0,'byteOffset':len(payload),'byteLength':len(image.getvalue())}]
        mapped = name.startswith('normal-')
        model['materials'][0].pop('normalTexture', None)
        model['images'] = model['images'][:1]
        model['textures'] = model['textures'][:1]
        panel = scene['scene']['children'][0]
        panel.pop('transform', None)
        if mapped:
            normal = (255,128,128) if name.endswith('-x') else (128,255,128)
            visibility = 0 if name.endswith('occluded') else 255
            surface = Image.new('RGBA', (32,16), (*normal,visibility))
            if name == 'normal-coverage':
                surface.paste((128,0,128,0),(16,0,32,16))
            encoded = io.BytesIO(); surface.save(encoded, format='PNG')
            padding = b'\0' * (-len(data) % 4)
            offset = len(data) + len(padding)
            data += padding + encoded.getvalue()
            model['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(encoded.getvalue())})
            model['images'].append({'bufferView':5,'mimeType':'image/png'})
            model['textures'].append({'source':1})
            model['materials'][0]['normalTexture'] = {'index':1}
            if name == 'normal-yawed-x':
                # The atlas frame follows the instance, never the camera-facing quad.
                panel['transform'] = {'rotation':[0,2**-.5,0,2**-.5]}
        model['buffers'] = [{'byteLength':len(data),
                             'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode()}]
        sun.update(direction=direction,intensity=intensity,color=color)
        settings.update(ambient=[.03]*3, iblEnabled=False,
                        skyboxTexture='', skyboxBlendTexture='', skyboxBlend=0)
        if name in ('normal-direct', 'normal-occluded') or name.startswith('normal-sky-'):
            settings['ambient'] = [0]*3
        if name.startswith('normal-sky-'):
            for sky, rgb in (('red',(240,8,8)),('blue',(8,8,240))):
                Image.new('RGB',(128,64),rgb).save(project/f'assets/skies/{sky}.png')
            settings.update(iblEnabled=True, iblDiffuseIntensity=1, iblSpecularIntensity=0,
                            skyboxTexture='assets/skies/blue.png' if name == 'normal-sky-blue'
                            else 'assets/skies/red.png')
            if name == 'normal-sky-blend':
                settings.update(skyboxBlendTexture='assets/skies/blue.png', skyboxBlend=.5)
        (project/'assets/models/panel.gltf').write_text(json.dumps(model))
        (project/'scenes/main.scene').write_text(json.dumps(scene))
        if not bundle.exists():
            subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(proj),
                            '--platform','windows','--out',str(bundle)], env=env,
                           check=True, stdout=subprocess.DEVNULL)
        else:
            for relative in ('scenes/main.scene','assets/models/panel.gltf'):
                (bundle/relative).write_bytes((project/relative).read_bytes())
            for sky in (project/'assets/skies').glob('*.png'):
                (bundle/'assets/skies').mkdir(parents=True, exist_ok=True)
                (bundle/'assets/skies'/sky.name).write_bytes(sky.read_bytes())
        capture = root/(name+'.png')
        with (root/(name+'.log')).open('w') as log:
            subprocess.run([str(bundle/'Billboard.exe'),'--screenshot',str(capture),
                            '--after-frames','30','--camera-pos',camera,'--camera-look','0,1,0'],
                           env=env,check=True,stdout=log,stderr=log,timeout=90)
        with Image.open(capture) as im:
            values[name] = ImageStat.Stat(im.convert('RGB').crop((314,174,326,186))).mean
    assert min(values['top']) > max(values['side']) + 20, values
    assert min(values['side']) > max(values['off']) + 20, values
    assert values['red'][0] > max(values['red'][1:]) + 50, values
    assert max(abs(a-b) for a,b in zip(values['top'],values['rotated'])) <= 1, values
    assert values['atlas-front'][0] > values['atlas-front'][1] + 60, values
    assert values['atlas-back'][1] > values['atlas-back'][0] + 60, values
    assert min(values['atlas-side'][:2]) > 50 and abs(values['atlas-side'][0]-values['atlas-side'][1]) <= 1, values
    assert min(values['normal-up']) > max(values['normal-away']) + 60, values
    assert max(abs(a-b) for a,b in zip(values['normal-up'],values['normal-up-side'])) <= 1, values
    # Hemisphere AO blocks sky SH, but cannot stand in for directional shadows.
    # The flat ambient fallback must match the mesh path across the LOD switch.
    assert max(abs(a-b) for a,b in zip(values['normal-occluded'],values['normal-direct'])) <= 1, values
    assert min(values['normal-ambient']) > 10, values
    assert max(abs(a-b) for a,b in zip(values['normal-ambient-occluded'],values['normal-ambient'])) <= 1, values
    assert max(abs(a-b) for a,b in zip(values['normal-ambient-occluded'],values['mesh-ambient'])) <= 1, values
    assert values['normal-sky-red'][0] > max(values['normal-sky-red'][1:]) + 60, values
    assert values['normal-sky-blue'][2] > max(values['normal-sky-blue'][:2]) + 60, values
    assert min(values['normal-sky-blend'][::2]) > 60, values
    assert abs(values['normal-sky-blend'][0]-values['normal-sky-blend'][2]) <= 1, values
    assert max(values['normal-sky-occluded']) <= 1, values
    assert max(abs(a-b) for a,b in zip(values['normal-up'],values['normal-coverage'])) <= 1, values
    assert min(values['normal-local-x']) > max(values['normal-yawed-x']) + 60, values
    print('PASS billboard orientation, atlas blend, authored normals/visibility and live directional lighting:',json.dumps(values))
    print('Captures:',root)


if __name__ == '__main__':
    main()
