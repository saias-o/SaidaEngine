"""Native GPU contracts for the GI refactor (Pillow required).

Build saida_gi_capture, then run this from the engine root:
  python tools/verify_global_illumination.py --build build-rel

Before changing rendering, record the GI-off reference ONCE:
  python tools/verify_global_illumination.py --build build-rel --cases off --record-off <new-directory>
Later runs use --reference <that-directory>. References are device/driver
specific and never regenerated implicitly. --icd selects a Vulkan ICD.
Every case writes its captures, logs, profile and metrics even if another fails.
Failures remain visible and never replace the frozen references implicitly.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageStat

WIDTH, HEIGHT = 640, 360
EYE, TARGET = (0., 2., 7.), (0., 1., 0.)
CASES = ('off', 'open', 'canyon', 'room', 'metal', 'optout', 'scroll', 'rebase', 'motion', 'stability')
ROOT = Path(__file__).resolve().parents[1]


def project(point, eye=EYE, target=TARGET):
    def unit(v):
        length = math.sqrt(sum(x * x for x in v))
        return [x / length for x in v]
    forward = unit([t - e for t, e in zip(target, eye)])
    right = unit([-forward[2], 0., forward[0]])
    up = [right[1]*forward[2]-right[2]*forward[1],
          right[2]*forward[0]-right[0]*forward[2],
          right[0]*forward[1]-right[1]*forward[0]]
    d = [p - e for p, e in zip(point, eye)]
    x, y, z = (sum(a*b for a, b in zip(d, axis)) for axis in (right, up, forward))
    focal = HEIGHT / (2 * math.tan(math.radians(60) / 2))
    if z <= 0:
        raise ValueError('sample is behind the fixture camera')
    px, py = round(WIDTH/2 + focal*x/z), round(HEIGHT/2 - focal*y/z)
    box = (px-4, py-4, px+4, py+4)
    if not (0 <= box[0] < box[2] <= WIDTH and 0 <= box[1] < box[3] <= HEIGHT):
        raise ValueError(f'sample outside the fixture: {box}')
    return box


def incident(pixel):
    """Invert the display's sRGB/ACES for energy comparisons in linear units."""
    v = pixel / 255.
    y = v / 12.92 if v <= .04045 else ((v + .055) / 1.055)**2.4
    # ACES(x) = (2.51*x*x + .03*x)/(2.43*x*x + .59*x + .14).
    a, b, c = 2.51 - 2.43*y, .03 - .59*y, -.14*y
    return (-b + math.sqrt(b*b - 4*a*c)) / (2*a)


def rgb(image, point=(0., 0., 0.), room=False):
    area = project(point, (0., 1.8, 1.5), (0., .2, -1.)) if room else project(point)
    # Convert each pixel before averaging: ACES/sRGB is nonlinear.
    patch = image.crop(area)
    pixels = list(patch.get_flattened_data() if hasattr(patch, 'get_flattened_data') else patch.getdata())
    return [sum(incident(p[c]) for p in pixels) / len(pixels) for c in range(3)]


def luma(value):
    return sum(a*b for a, b in zip(value, (.2126, .7152, .0722)))


def image_digest(image):
    return hashlib.sha256(image.tobytes()).hexdigest()


class Run:
    def __init__(self, args):
        self.args = args
        self.build = args.build.resolve()
        runs = self.build / 'global-illumination-run'
        runs.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix='run-', dir=runs))
        self.env = dict(os.environ, SAIDA_WINDOW_HIDDEN='1', SAIDA_WINDOW_SIZE=f'{WIDTH}x{HEIGHT}')
        if args.icd:
            self.env.update(VK_ICD_FILENAMES=str(args.icd.resolve()), VK_DRIVER_FILES=str(args.icd.resolve()))
        self.images = {}
        self.results = {}

    def capture(self, name, scene='canyon-red', gi=True, **params):
        if name in self.images:
            return self.images[name]
        fixture = dict(scene=scene, gi=gi, frames=self.args.frames,
                       image=str(self.root / f'{name}.png'), profile=str(self.root / f'{name}.json'))
        fixture.update(params)
        request = self.root / f'{name}.fixture.json'
        request.write_text(json.dumps(fixture, indent=2), encoding='utf-8')
        executable = self.build / 'tests/saida_gi_capture.exe'
        if not executable.exists():
            executable = self.build / 'tests/saida_gi_capture'
        with (self.root / f'{name}.log').open('w', encoding='utf-8') as log:
            subprocess.run([str(executable), str(request)], env=self.env, check=True,
                           stdout=log, stderr=subprocess.STDOUT, timeout=240)
        image = Image.open(fixture['image']).convert('RGB')
        if image.size != (WIDTH, HEIGHT):
            raise AssertionError(f'capture size {image.size}, expected {(WIDTH, HEIGHT)}')
        # A closed room under flat ambient, or a correctly dark metal, may be
        # uniform. Visible geometry is checked in the open/off control instead.
        if scene == 'open' and max(ImageStat.Stat(image).stddev) < 2:
            raise AssertionError(f'empty open-plane capture: {name}')
        self.images[name] = image
        return image

    def off(self):
        images = {scene: self.capture(f'off-{scene}', scene, False)
                  for scene in ('open', 'canyon-red', 'room', 'metal')}
        log = (self.root / 'off-open.log').read_text(encoding='utf-8', errors='replace')
        gpu = next((line for line in log.splitlines() if 'GPU:' in line), 'unknown GPU')
        metadata = dict(schema=1, gpu=gpu, dimensions=[WIDTH, HEIGHT], frames=self.args.frames,
                        corpus='GlobalIlluminationCapture.cpp phase 0',
                        pixels={key: image_digest(im) for key, im in images.items()})
        if self.args.record_off:
            reference = self.args.record_off.resolve()
            # An existing reference may never be overwritten by a verification run.
            reference.mkdir(parents=True, exist_ok=False)
            for key, im in images.items():
                im.save(reference / f'{key}.png')
            (reference / 'reference.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
            return dict(recorded=str(reference), **metadata)
        if not self.args.reference:
            raise AssertionError('no GI-off reference: record it on the unmodified renderer first')
        reference = self.args.reference.resolve()
        original = json.loads((reference / 'reference.json').read_text(encoding='utf-8'))
        if original['gpu'] != gpu:
            raise AssertionError(f'GPU mismatch: {gpu!r} vs {original["gpu"]!r}')
        for key, image in images.items():
            with Image.open(reference / f'{key}.png') as source:
                source = source.convert('RGB')
                if image_digest(source) != original['pixels'][key]:
                    raise AssertionError(f'corrupted reference: {key}')
                if source.size != image.size or ImageChops.difference(image, source).getbbox():
                    raise AssertionError(f'GI-off pixels changed: {key}')
        return metadata

    def open(self):
        off = rgb(self.capture('off-open', 'open', False))
        on = rgb(self.capture('open-on', 'open'))
        ratio = luma(on) / max(luma(off), 1e-8)
        # Also exercise a real uniform environment with diffuse SH enabled;
        # flat ambient alone would miss the old double-sky composition defect.
        mantissa, exponent = math.frexp(.2)
        component = int(mantissa*256)
        sky = self.root / 'uniform-sky.hdr'
        sky.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 64 +X 128\n'
                        + bytes([component]*3+[exponent+128])*(128*64))
        ibl_off = rgb(self.capture('ibl-off', 'open', False, sky=str(sky)))
        ibl_on = rgb(self.capture('ibl-on', 'open', True, sky=str(sky)))
        ibl_ratio = luma(ibl_on)/max(luma(ibl_off), 1e-8)
        metrics = dict(off=off, on=on, ratio=ratio, ibl_off=ibl_off, ibl_on=ibl_on, ibl_ratio=ibl_ratio)
        assert luma(off) > .02, f'open-sky fixture is too dark: {off}'
        assert luma(ibl_off) > .02, f'diffuse SH fixture is not loaded: {ibl_off}'
        assert abs(ratio-1) <= .02 and abs(ibl_ratio-1) <= .02, \
            f'open-sky energy must match GI-off within 2% (ambient and IBL): {metrics}'
        return metrics

    def canyon(self):
        red = rgb(self.capture('canyon-red'), (-2., 1., 0.))
        blue = rgb(self.capture('canyon-blue', 'canyon-blue'), (-2., 1., 0.))
        off = rgb(self.capture('off-canyon-red', gi=False), (-2., 1., 0.))
        floor = rgb(self.images['canyon-red'])
        open_floor = rgb(self.capture('open-on', 'open'))
        assert red[0] > 1.15*red[2] and blue[2] > 1.15*blue[0], f'no coloured bounce: {red}, {blue}'
        assert red[0] > off[0]+.001, f'red bounce did not light the shadowed receiver: {red}, {off}'
        assert luma(floor) < .9*luma(open_floor), f'canyon floor is not sky-occluded: {floor}, {open_floor}'
        return dict(red=red, blue=blue, off=off, floor=floor, open=open_floor)

    def room(self):
        off = rgb(self.capture('off-room', 'room', False), (1., 0., -1.), room=True)
        on = rgb(self.capture('room-on', 'room'), (1., 0., -1.), room=True)
        assert luma(off) > .02, f'room fixture is not visible: {off}'
        assert luma(on) <= .1*luma(off), f'exterior light leaks deep inside the room: {on}, off {off}'
        return dict(off=off, on=on)

    def metal(self):
        value = rgb(self.capture('metal-on', 'metal'))
        assert max(value) < .001, f'metal receives diffuse indirect light: {value}'
        return dict(on=value)

    def optout(self):
        off = self.capture('off-canyon-red', gi=False)
        on = self.capture('optout-on', optOut=True)
        area = project((-2., 1., 0.))
        assert ImageChops.difference(off.crop(area), on.crop(area)).getbbox() is None, \
            'material opt-out differs from the legacy GI-off receiver'
        return dict(receiver=list(area), sha256=image_digest(on.crop(area)))

    def scroll(self):
        initial = rgb(self.capture('scroll-initial', follow=True), (-2., 1., 0.))
        # A camera-relative replica keeps expected lighting constant. Real camera
        # and node motion happen in the same running engine, after initial warmup.
        values = []
        for distance in (100., 250., 500.):
            value = rgb(self.capture(f'scroll-{distance:g}', follow=True, travel=distance), (-2., 1., 0.))
            values.append(value)
        # Capture adjacent frames straddling a one-cell move, after convergence.
        before = rgb(self.capture('scroll-before', follow=True, frames=self.args.frames, motionFrame=self.args.frames+1), (-2., 1., 0.))
        after = rgb(self.capture('scroll-after', follow=True, frames=self.args.frames, motionFrame=self.args.frames,
                                 travel=2.), (-2., 1., 0.))
        delta = abs(luma(after)-luma(before))/max(luma(before), 1e-8)
        assert initial[0] > 1.15*initial[2], f'initial fixture has no bounce to preserve: {initial}'
        for value in values:
            assert max(abs(a-b) for a, b in zip(initial, value)) <= .02*max(initial), \
                f'coverage/lighting lost while scrolling: {initial} -> {values}'
        assert delta <= .05, f'scrolling luminance jump: {delta:.3%}'
        return dict(initial=initial, translated=values, adjacent_frame_delta=delta)

    def rebase(self):
        initial = rgb(self.capture('rebase-initial', follow=True), (-2., 1., 0.))
        values = [rgb(self.capture(f'rebase-{angle:g}', follow=True, rebase=True, travel=-350., rotation=angle),
                      (-2., 1., 0.)) for angle in (0., .003)]
        for value in values:
            assert max(abs(a-b) for a, b in zip(initial, value)) <= .02*max(initial), \
                f'origin translation/rotation changed irradiance: {initial} -> {values}'
        return dict(initial=initial, rebased=values)

    def motion(self):
        # Move ONLY the camera along static walls. Moving the scene with the
        # eye tests teleport recovery, not whether light is ready on approach.
        end = max(self.args.frames, 240)
        metrics = {}
        for speed in (6, 24):
            name = f'motion-{speed}'
            first = self.capture(name, follow=True, frames=end-1, sampleFrames=[end, end+240],
                                 motionFrame=end-119, motionFrames=120, cameraTravel=speed*2)
            images = {end-1: first, **{frame: Image.open(self.root / f'{name}-{frame}.png').convert('RGB')
                                      for frame in (end, end+240)}}
            values = {frame: [luma(rgb(image, (-2., 1., z))) for z in (0., -2., -4.)]
                      for frame, image in images.items()}
            settled = values[end+240]
            assert min(settled) > .001, f'motion fixture has no indirect light: {settled}'
            drift = [abs(a-b)/b for a, b in zip(values[end], settled)]
            jump = [abs(a-b)/max(b, 1e-8) for a, b in zip(values[end], values[end-1])]
            metrics[speed] = dict(moving=values[end], settled=settled,
                                  settling_change=drift, adjacent_change=jump)
            assert max(drift) < .20, f'GI is still loading at {speed} m/s: {metrics[speed]}'
            assert max(jump) < .08, f'GI jumps during approach at {speed} m/s: {metrics[speed]}'
        return metrics

    def stability(self):
        # Maintenance updates with fixed geometry, light and eye.
        # Measure whole visible surfaces, including probe-classification boundaries.
        metrics = {}
        failures = []
        for follow in (False, True):
            name = f'still-{follow}'
            reference = self.capture(name, follow=follow, frames=720,
                                     sampleFrames=[744, 768, 792, 1032], motionFrame=793, lightScale=0.)
            images = [Image.open(self.root / f'{name}-{frame}.png').convert('RGB')
                      for frame in (744, 768, 792)]
            differences = [ImageChops.difference(reference, image) for image in images]
            peak = max(max(channel[1] for channel in diff.getextrema()) for diff in differences)
            mean = max(max(ImageStat.Stat(diff).mean) for diff in differences)
            metrics[str(follow)] = dict(peak_display_delta=peak, mean_display_delta=mean)
            if peak > 3 or mean > .1:
                failures.append(f'stationary GI flickers (follow={follow})')
            dark = Image.open(self.root / f'{name}-1032.png').convert('RGB')
            before = luma(rgb(reference, (-2., 1., 0.)))
            after = luma(rgb(dark, (-2., 1., 0.)))
            metrics[str(follow)].update(lit_receiver=before, unlit_receiver=after)
            if before <= .001 or after >= before*.5:
                failures.append(f'stationary probes stopped responding to light changes (follow={follow})')
        assert not failures, f'{failures}: {metrics}'
        return metrics

    def run(self, cases):
        for case in cases:
            try:
                metrics = getattr(self, case)()
                result = dict(passed=True, metrics=metrics)
            except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
                result = dict(passed=False, error=str(error))
            self.results[case] = result
            print(f'{"PASS" if result["passed"] else "FAIL"} GI {case}: {json.dumps(result)}', flush=True)
            (self.root / 'report.json').write_text(json.dumps(self.results, indent=2), encoding='utf-8')
        print('Captures, logs and profiles:', self.root, flush=True)
        return all(value['passed'] for value in self.results.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build-rel'))
    parser.add_argument('--icd', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--record-off', type=Path)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    args = parser.parse_args()
    if args.frames < 121:
        parser.error('--frames must be at least 121 to include initial warmup and camera motion')
    if args.reference and args.record_off:
        parser.error('--reference and --record-off are mutually exclusive')
    if args.record_off and args.cases != ['off']:
        parser.error('--record-off requires --cases off; inspect references before running other cases')
    if not args.record_off and not args.reference:
        args.reference = ROOT / 'tests/fixtures/global-illumination/mx450'
    raise SystemExit(0 if Run(args).run(args.cases) else 1)


if __name__ == '__main__':
    main()
