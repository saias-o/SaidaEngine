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
    for folder in ('scenes', 'assets/models'):
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
        'atlas-back':([0,-1,0],2,[1,1,1],'0,1,-5',True)}
    for name,(direction,intensity,color,camera,colored) in cases.items():
        atlas = Image.new('RGBA',(32,16),(160,160,160,255))
        if colored:
            atlas.paste((160,0,0,255),(0,0,16,16))
            atlas.paste((0,160,0,255),(16,0,32,16))
        image = io.BytesIO(); atlas.save(image,format='PNG')
        data = payload + image.getvalue()
        model['bufferViews'] = model['bufferViews'][:4] + [
            {'buffer':0,'byteOffset':len(payload),'byteLength':len(image.getvalue())}]
        model['buffers'] = [{'byteLength':len(data),
                             'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode()}]
        sun.update(direction=direction,intensity=intensity,color=color)
        (project/'assets/models/panel.gltf').write_text(json.dumps(model))
        (project/'scenes/main.scene').write_text(json.dumps(scene))
        if not bundle.exists():
            subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(proj),
                            '--platform','windows','--out',str(bundle)], env=env,
                           check=True, stdout=subprocess.DEVNULL)
        else:
            for relative in ('scenes/main.scene','assets/models/panel.gltf'):
                (bundle/relative).write_bytes((project/relative).read_bytes())
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
    print('PASS billboard orientation, atlas blend and live directional intensity/color/elevation:',json.dumps(values))
    print('Captures:',root)


if __name__ == '__main__':
    main()
