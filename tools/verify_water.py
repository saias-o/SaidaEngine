"""Capture realistic water at fixed viewpoints and exercise semantic regressions.

Run from the engine directory: python tools/verify_water.py --label before
Outputs are deliberately outside the source tree, under the selected build.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from PIL import Image, ImageChops, ImageStat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-rel'))
    parser.add_argument('--label', default='current')
    parser.add_argument('--sky', type=Path)
    parser.add_argument('--checks', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    root = build / 'water-verification' / args.label
    source = root / 'source'
    (source / 'scenes').mkdir(parents=True, exist_ok=True)
    (source / 'assets').mkdir(exist_ok=True)
    sky = args.sky or Path('BeachDemo/assets/skies/sunset.png')
    sky_name = 'sky' + sky.suffix
    shutil.copyfile(sky, source / 'assets' / sky_name)
    settings = dict(changeRenderingAtLoad=True, giEnabled=False, ambient=[.12,.15,.19],
                    iblEnabled=True, iblDiffuseIntensity=1, iblSpecularIntensity=1,
                    skyboxTexture='assets/' + sky_name, skyboxExposure=1,
                    aoEnabled=False, bloomEnabled=True, fogEnabled=False)
    water = dict(type='Water', name='Ocean', id=3, enabled=True, behaviours=[], children=[],
                 size=24000, amplitude=.25, wavelength=15, reflectivity=.8)
    sun = dict(type='LightNode', name='Sun', id=2, enabled=True, behaviours=[], children=[],
               lightType=0, direction=[-.35,-.28,.89], color=[1,.83,.66], intensity=3,
               castShadows=False)
    scene = dict(schema=2, version=2, scene=dict(type='Scene', name='Water verification', id=1,
                 enabled=True, behaviours=[], children=[sun, water, dict(type='Camera',name='Capture lens',id=4,
                 enabled=True,active=True,farZ=100000,nearZ=.1,behaviours=[],children=[])], settings=settings))
    project = source / 'WaterCheck.saidaproj'
    project.write_text(json.dumps(dict(schema=1,version=1,name='WaterCheck',engineVersion='1.0.0',
                                      mainScene='scenes/main.scene')),encoding='utf-8')
    env = dict(os.environ, SAIDA_WINDOW_HIDDEN='1', SAIDA_WINDOW_SIZE='1280x720')

    def export():
        (source/'scenes/main.scene').write_text(json.dumps(scene),encoding='utf-8')
        subprocess.run([str(build/'bin/saida_tool.exe'),'export-game',str(project),
                        '--platform','windows','--out',str(root/'player')],
                       env=env,check=True,stdout=subprocess.DEVNULL)

    def capture(name, position, look, frame=60):
        png=root/(name+'.png')
        with (root/(name+'.log')).open('w') as log:
            subprocess.run([str(root/'player/WaterCheck.exe'),'--screenshot',str(png),
                            '--after-frames',str(frame),'--camera-pos',position,'--camera-look',look,
                            '--profile',str(root/(name+'-profile.json'))],
                           env=env,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=120)
        with Image.open(png) as image:
            return image.convert('RGB')

    export()
    capture('close','0,3,12','0,0,-45')
    capture('coast-height','0,35,100','0,0,-150')
    aerial=capture('aerial','0,1800,1000','0,0,-400')
    if args.checks:
        later=capture('aerial-later','0,1800,1000','0,0,-400',240)
        delta=sum(ImageStat.Stat(ImageChops.difference(aerial,later)).mean)/3
        assert delta > .03, ('frozen aerial water',delta)
        profiles=[]
        for profile,name in ((0,'swell'),(1,'wind-sea'),(2,'chop')):
            water['waveType']=profile
            export()
            profiles.append(capture(name,'0,6,12','0,0,-40'))
        assert all(sum(ImageStat.Stat(ImageChops.difference(profiles[0],other)).mean)/3 > .5
                   for other in profiles[1:]), 'wave profiles have no visible effect'
        water['waveType']=1
        water['waveIntensity']=0
        export()
        still=capture('intensity-zero','0,6,12','0,0,-40')
        still_later=capture('intensity-zero-later','0,6,12','0,0,-40',240)
        assert sum(ImageStat.Stat(ImageChops.difference(still,still_later)).mean)/3 < .01, 'zero intensity still animates'
        water['waveIntensity']=2
        export()
        strong=capture('intensity-two','0,6,12','0,0,-40')
        assert sum(ImageStat.Stat(ImageChops.difference(strong,profiles[1])).mean)/3 > .5, 'intensity has no effect'
        water['waveIntensity']=1
        # Change only triangulation, retaining the same planar coverage and
        # nonzero waves/foam. The old vertex displacement and crest mask made
        # these two equivalent surfaces visibly different.
        water['foamIntensity']=.7
        water['surface']='-24000 0 -24000 24000 0 -24000 24000 0 24000 -24000 0 -24000 24000 0 24000 -24000 0 24000'
        export()
        diagonal_a=capture('triangles-a','0,120,70','0,0,0')
        water['surface']='-24000 0 -24000 24000 0 -24000 -24000 0 24000 24000 0 -24000 24000 0 24000 -24000 0 24000'
        export()
        diagonal_b=capture('triangles-b','0,120,70','0,0,0')
        seam_delta=sum(ImageStat.Stat(ImageChops.difference(diagonal_a,diagonal_b)).mean)/3
        assert seam_delta < .4, ('shading depends on triangulation',seam_delta)
        del water['surface']
        # A shaped surface with arbitrary large triangles must not reveal the
        # tessellation through vertex crests, foam or wave displacement.
        water['amplitude']=0
        water['detailStrength']=0
        water['detail2Strength']=0
        water['foamIntensity']=0
        sun['intensity']=0
        settings['bloomEnabled']=False
        Image.new('RGB',(128,64),(240,4,4)).save(source/'assets/red.png')
        Image.new('RGB',(128,64),(4,4,240)).save(source/'assets/blue.png')
        colors=[]
        for name in ('red','blue'):
            settings['skyboxTexture']='assets/'+name+'.png'
            export()
            img=capture(name,'0,3,12','0,0,-45')
            colors.append(ImageStat.Stat(img.crop((200,400,1080,650))).mean)
        assert colors[0][0]>colors[1][0]+20 and colors[1][2]>colors[0][2]+20,colors
        settings['iblEnabled']=False
        settings['ambient']=[0,0,0]
        scene['scene']['children'].remove(sun)
        export()
        dark=capture('unlit','0,3,12','0,0,-45')
        assert max(ImageStat.Stat(dark.crop((200,400,1080,650))).mean) < 1, 'water emits light without illumination'
        print('PASS animation, wave profiles/intensities, triangulation invariance, environment reflection and unlit water',delta,seam_delta,colors)
    print('Captures:',root)


if __name__ == '__main__':
    main()
