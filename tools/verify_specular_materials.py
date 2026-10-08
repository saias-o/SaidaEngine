"""Pixel regression for glTF dielectric IOR/specular factors and textures.

Run with Pillow: python tools/verify_specular_materials.py --build build-rel
Ambient, diffuse IBL, GI and direct lights are disabled to isolate reflectance.
"""
import argparse,base64,copy,io,json,os,struct,subprocess,tempfile
from pathlib import Path
from PIL import Image,ImageStat

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build-rel'))
    args=parser.parse_args();build=args.build.resolve()
    runs=build/'specular-material-run';runs.mkdir(exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='run-',dir=runs));project=root/'source'
    for folder in ('scenes','assets/models','assets/skies'):(project/folder).mkdir(parents=True,exist_ok=True)
    positions=struct.pack('<12f',-.75,-.75,0,.75,-.75,0,.75,.75,0,-.75,.75,0)
    payload=positions+struct.pack('<12f',*([0,0,1]*4))+struct.pack('<6H',0,1,2,0,2,3)
    model={'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0]}],
        'buffers':[{'byteLength':len(payload),'uri':'data:application/octet-stream;base64,'+base64.b64encode(payload).decode()}],
        'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':48},{'buffer':0,'byteOffset':48,'byteLength':48},{'buffer':0,'byteOffset':96,'byteLength':12}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[-.75,-.75,0],'max':[.75,.75,0]},
                     {'bufferView':1,'componentType':5126,'count':4,'type':'VEC3'},
                     {'bufferView':2,'componentType':5123,'count':6,'type':'SCALAR'}],
        'materials':[{'pbrMetallicRoughness':{'baseColorFactor':color,'metallicFactor':metal,'roughnessFactor':rough}}
                     for color,metal,rough in (([.8,.8,.8,1],1.,.12),([.015,.015,.015,1],0.,.84))],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2,'material':i}]} for i in range(2)],
        'nodes':[{'mesh':0,'translation':[0,1,0]}]}
    (project/'assets/models/panels.gltf').write_text(json.dumps(model),encoding='utf-8')
    model['materials']=[{'pbrMetallicRoughness':{'baseColorFactor':[0,0,0,1],'metallicFactor':0,'roughnessFactor':.3}}]
    model['meshes']=model['meshes'][:1]
    # Texcoords are present so both color and alpha textures sample an authored UV.
    offset=len(payload);payload+=struct.pack('<8f',*([.5,.5]*4))
    model['buffers'][0]={'byteLength':len(payload),'uri':'data:application/octet-stream;base64,'+base64.b64encode(payload).decode()}
    model['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':32})
    model['accessors'].append({'bufferView':3,'componentType':5126,'count':4,'type':'VEC2'})
    model['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']=3
    model['extensionsUsed']=['KHR_materials_specular','KHR_materials_ior']
    texture=io.BytesIO();Image.new('RGBA',(2,2),(128,128,128,128)).save(texture,format='PNG')
    textureOffset=len(payload);payload+=texture.getvalue()
    model['buffers'][0]={'byteLength':len(payload),'uri':'data:application/octet-stream;base64,'+base64.b64encode(payload).decode()}
    model['bufferViews'].append({'buffer':0,'byteOffset':textureOffset,'byteLength':len(texture.getvalue())})
    model['images']=[{'bufferView':4,'mimeType':'image/png'}]
    model['textures']=[{'source':0}]
    settings={'changeRenderingAtLoad':True,'giEnabled':False,'ambient':[0,0,0],
              'iblEnabled':True,'iblDiffuseIntensity':0,'iblSpecularIntensity':1,
              'skyboxTexture':'assets/skies/grey.hdr','aoEnabled':False,'bloomEnabled':False,'fogEnabled':False}
    (project/'assets/skies/grey.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 64 +X 128\n'+bytes([128,128,128,129])*(128*64))
    scene={'schema':2,'version':2,'scene':{'type':'Scene','name':'Specular','enabled':True,
           'children':[{'type':'Node','name':'Panel','importedFrom':'assets/models/panel.gltf'}],'settings':settings}}
    (project/'scenes/main.scene').write_text(json.dumps(scene),encoding='utf8')
    proj=project/'Specular.saidaproj'
    proj.write_text(json.dumps({'schema':1,'version':1,'name':'Specular','engineVersion':'1.0.0','mainScene':'scenes/main.scene'}),encoding='utf8')
    env=dict(os.environ,SAIDA_WINDOW_HIDDEN='1',SAIDA_WINDOW_SIZE='640x360')
    base=copy.deepcopy(model['materials'][0]);bundle=root/'player';values={}
    cases={'core':{},'disabled':{'KHR_materials_specular':{'specularFactor':0}},
           'ior-one':{'KHR_materials_ior':{'ior':1}},
           'ior-three':{'KHR_materials_ior':{'ior':3}},
           'color-texture':{'KHR_materials_specular':{'specularColorTexture':{'index':0}}},
           'strength-texture':{'KHR_materials_specular':{'specularTexture':{'index':0}}},
           'colored':{'KHR_materials_specular':{'specularColorFactor':[1,.2,.05]}},
           'metal-core':{},'metal-disabled':{'KHR_materials_ior':{'ior':1},'KHR_materials_specular':{'specularFactor':0}}}
    for name,extensions in cases.items():
        model['materials'][0]=copy.deepcopy(base);model['materials'][0]['extensions']=extensions
        if name.startswith('metal-'):
            model['materials'][0]['pbrMetallicRoughness'].update(baseColorFactor=[.8,.8,.8,1],metallicFactor=1)
        path=project/'assets/models/panel.gltf';path.write_text(json.dumps(model),encoding='utf8')
        if not bundle.exists():
            subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(proj),'--platform','windows','--out',str(bundle)],env=env,check=True,stdout=subprocess.DEVNULL)
        else:
            (bundle/'assets/models/panel.gltf').write_text(path.read_text(),encoding='utf8')
        png=root/(name+'.png')
        with (root/(name+'.log')).open('w') as log:
            subprocess.run([str(bundle/'Specular.exe'),'--screenshot',str(png),'--after-frames','30','--camera-pos','0,1,5','--camera-look','0,1,0'],env=env,check=True,stdout=log,stderr=log,timeout=90)
        with Image.open(png) as im:values[name]=ImageStat.Stat(im.convert('RGB').crop((314,174,326,186))).mean
    assert min(values['core'])>20,values
    # IOR 1 removes F0; a small off-axis Fresnel term remains on the plane.
    assert max(values['disabled'])<2 and max(values['ior-one'])<min(values['core'])/10,values
    assert min(values['ior-three'])>max(values['core'])+30,values
    assert min(values['color-texture'])>2 and max(values['color-texture'])<min(values['strength-texture'])-8,values
    assert max(values['strength-texture'])<min(values['core'])-5,values
    r,g,b=values['colored'];assert r>g+10 and g>b+5,values
    assert min(values['metal-core'])>100 and max(abs(a-b) for a,b in zip(values['metal-core'],values['metal-disabled']))<=1,values
    print('PASS dielectric specular/IOR pixel semantics:',json.dumps(values))
    print('Captures:',root)

if __name__=='__main__':main()
