"""Semantic pixel regression: environment lighting and display transfer.

python tools/verify_ibl_environment.py --build build-rel [--icd path/to/lvp.json]
Red/blue skies must change the metal while a dark dielectric stays much darker.
HDR greys receive exactly one sRGB encoding. Diffuse and specular IBL follow
both skies, their crossfade and independent rotations. No golden image or
GPU-specific tolerance; ambient, GI and direct lights are off.
"""
import argparse
import base64
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image, ImageStat


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build-rel'))
    parser.add_argument('--icd',type=Path)
    args=parser.parse_args();build=args.build.resolve()
    runs=build/'ibl-environment-run';runs.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='run-',dir=runs));project=root/'source'
    for folder in ('scenes','assets/models','assets/skies'):(project/folder).mkdir(parents=True,exist_ok=True)
    positions=struct.pack('<12f',-.75,-.75,0,.75,-.75,0,.75,.75,0,-.75,.75,0)
    payload=positions+struct.pack('<12f',*([0,0,1]*4))+struct.pack('<6H',0,1,2,0,2,3)
    model={'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0,1]}],
        'buffers':[{'byteLength':len(payload),'uri':'data:application/octet-stream;base64,'+base64.b64encode(payload).decode()}],
        'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':48},{'buffer':0,'byteOffset':48,'byteLength':48},{'buffer':0,'byteOffset':96,'byteLength':12}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[-.75,-.75,0],'max':[.75,.75,0]},
                     {'bufferView':1,'componentType':5126,'count':4,'type':'VEC3'},
                     {'bufferView':2,'componentType':5123,'count':6,'type':'SCALAR'}],
        'materials':[{'pbrMetallicRoughness':{'baseColorFactor':color,'metallicFactor':metal,'roughnessFactor':rough}}
                     for color,metal,rough in (([.8,.8,.8,1],1.,.12),([.015,.015,.015,1],0.,.84))],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2,'material':i}]} for i in range(2)],
        'nodes':[{'mesh':0,'translation':[-1.1,1,0]},{'mesh':1,'translation':[1.1,1,0]}]}
    (project/'assets/models/panels.gltf').write_text(json.dumps(model),encoding='utf-8')
    settings={'changeRenderingAtLoad':True,'giEnabled':False,'ambient':[0,0,0],
              'iblEnabled':True,'iblDiffuseIntensity':0,'iblSpecularIntensity':1,
              'skyboxExposure':1,'skyboxBlend':0,'aoEnabled':False,'bloomEnabled':False,'fogEnabled':False}
    node={'type':'Node','name':'Metal and rubber','id':2,'enabled':True,'behaviours':[],
          'children':[],'importedFrom':'assets/models/panels.gltf'}
    scene={'schema':2,'version':2,'scene':{'type':'Scene','name':'IBL','id':1,
           'enabled':True,'behaviours':[],'children':[node],'settings':settings}}
    project_file=project/'IBL.saidaproj'
    project_file.write_text(json.dumps({'schema':1,'version':1,'name':'IBL','engineVersion':'0.1.0','mainScene':'scenes/main.scene'}),encoding='utf-8')
    env=dict(os.environ,SAIDA_WINDOW_HIDDEN='1',SAIDA_WINDOW_SIZE='640x360')
    if args.icd:env['VK_ICD_FILENAMES']=str(args.icd.resolve())
    readings={}
    for name,color in (('red',(240,8,8)),('blue',(8,8,240))):
        Image.new('RGB',(128,64),color).save(project/f'assets/skies/{name}.png')
        settings['skyboxTexture']=f'assets/skies/{name}.png'
        (project/'scenes/main.scene').write_text(json.dumps(scene),encoding='utf-8')
        bundle=root/name
        subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(project_file),
                        '--platform','windows','--out',str(bundle)],env=env,check=True,stdout=subprocess.DEVNULL)
        png=root/f'{name}.png'
        with (root/f'{name}.log').open('w',encoding='utf-8') as log:
            subprocess.run([str(bundle/'IBL.exe'),'--screenshot',str(png),'--after-frames','30',
                            '--camera-pos','0,1,5','--camera-look','0,1,0'],
                           env=env,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
        with Image.open(png) as source:
            rgb=source.convert('RGB')
            # Centers of the two authored panels with the default 60-degree FOV.
            readings[name]=[ImageStat.Stat(rgb.crop(box)).mean for box in ((238,174,258,186),(382,174,402,186))]
        metal,rubber=readings[name];channel=0 if name=='red' else 2
        assert metal[channel]>100 and metal[channel]>3*max(metal[j] for j in range(3) if j!=channel),readings
        assert metal[channel]>rubber[channel]+40,readings
    assert readings['red'][0][0]>readings['blue'][0][0]+60,readings
    assert readings['blue'][0][2]>readings['red'][0][2]+60,readings
    print('PASS PBR environment binding:',readings)

    # An HDR grey sky isolates the display transfer from BRDFs and texture
    # decoding. Compare pixels with ACES followed by exactly one IEC sRGB
    # transfer, including the dark toe where a gamma-2.2 approximation differs.
    def aces_srgb(value):
        mapped=max(0.,min(1.,value*(2.51*value+.03)/(value*(2.43*value+.59)+.14)))
        return 255*(12.92*mapped if mapped<=.0031308 else 1.055*mapped**(1/2.4)-.055)

    def capture(name):
        (project/'scenes/main.scene').write_text(json.dumps(scene),encoding='utf-8')
        bundle=root/name
        subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(project_file),
                        '--platform','windows','--out',str(bundle)],env=env,check=True,stdout=subprocess.DEVNULL)
        png=root/f'{name}.png'
        with (root/f'{name}.log').open('w',encoding='utf-8') as log:
            subprocess.run([str(bundle/'IBL.exe'),'--screenshot',str(png),'--after-frames','30',
                            '--camera-pos','0,1,5','--camera-look','0,1,0'],
                           env=env,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=90)
        with Image.open(png) as source:
            rgb=source.convert('RGB')
            return [ImageStat.Stat(rgb.crop(box)).mean for box in
                    ((238,174,258,186),(382,174,402,186),(10,10,30,30))]

    for value in (.003,.18,1.):
        mantissa,exponent=math.frexp(value)
        component=int(mantissa*256)
        radiance=component*2.**(exponent-8)
        hdr=project/'assets/skies/grey.hdr'
        hdr.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 64 +X 128\n'
                        +bytes([component]*3+[exponent+128])*(128*64))
        settings['skyboxTexture']='assets/skies/grey.hdr'
        pixels=capture(f'transfer-{value}')[2]
        expected=aces_srgb(radiance)
        assert max(abs(channel-expected) for channel in pixels)<3,(value,pixels,expected)
    print('PASS HDR display transfer: one sRGB encoding')

    # The sky background, specular IBL and diffuse SH must all follow the
    # same crossfade. A blue endpoint cannot leave red reflections behind.
    settings['skyboxTexture']='assets/skies/red.png'
    settings['skyboxBlendTexture']='assets/skies/blue.png'
    for blend in (.5,1.):
        settings['skyboxBlend']=blend
        metal,_,sky=capture(f'blend-specular-{blend}')
        if blend==.5:
            assert min(metal[0],metal[2])>60 and abs(metal[0]-metal[2])<3,(metal,sky)
        else:
            assert max(abs(metal[i]-readings['blue'][0][i]) for i in range(3))<3,(metal,readings)
    settings['iblDiffuseIntensity']=1
    settings['iblSpecularIntensity']=0
    model['materials'][1]['pbrMetallicRoughness']['baseColorFactor']=[.25,.25,.25,1]
    (project/'assets/models/panels.gltf').write_text(json.dumps(model),encoding='utf-8')
    for blend in (.5,1.):
        settings['skyboxBlend']=blend
        _,diffuse,_=capture(f'blend-diffuse-{blend}')
        if blend==.5:
            assert min(diffuse[0],diffuse[2])>60 and abs(diffuse[0]-diffuse[2])<3,diffuse
        else:
            assert diffuse[2]>100 and diffuse[2]>3*diffuse[0],diffuse
    print('PASS specular/diffuse environment crossfade')

    directional=Image.new('RGB',(128,64),(0,0,0))
    directional.paste((8,8,240),(64,0,128,64))
    directional.save(project/'assets/skies/directional.png')
    settings['skyboxBlendTexture']='assets/skies/directional.png'
    settings['skyboxBlend']=1
    for kind in ('specular','diffuse'):
        settings['iblDiffuseIntensity']=1 if kind=='diffuse' else 0
        settings['iblSpecularIntensity']=1 if kind=='specular' else 0
        rotated=[]
        for angle in (0,math.pi):
            settings['skyboxRotation']=math.pi/2  # distinct from secondary
            settings['skyboxBlendRotation']=angle
            pixels=capture(f'rotation-{kind}-{angle}')
            rotated.append(pixels[0 if kind=='specular' else 1][2])
        assert rotated[0]>100 and rotated[1]<10,rotated
    print('PASS independent secondary sky rotations')

    # No second sky: a nonzero authored blend must still use the primary alone.
    settings['skyboxBlendTexture']=''
    settings['iblDiffuseIntensity']=0
    settings['iblSpecularIntensity']=1
    metal,_,sky=capture('missing-secondary')
    assert max(abs(metal[i]-readings['red'][0][i]) for i in range(3))<3,(metal,readings)
    assert sky[0]>200 and sky[0]>3*sky[2],sky
    print('PASS missing secondary sky fallback')
    print('Captures:',root)


if __name__=='__main__':main()
