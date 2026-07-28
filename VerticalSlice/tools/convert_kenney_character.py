"""Blender headless: build one skinned GLB from Kenney's character kit.

    blender -b --python convert_kenney_character.py -- <modelFbx> <animDir> <skinPng> <outGlb>

Kenney ships the rig and each animation as separate FBX files, and SaidaEngine's
Animator only sees the clips inside the ONE file a node imports — so they have to
be merged. Two things make that less trivial than copying actions across:

1. The animation files' armatures do not share the model's rest pose: all 58
   bones sit elsewhere, because the animation FBX carries its first animated
   frame where the model carries the bind pose. A Blender action stores bone
   transforms RELATIVE to the rest pose, so assigning the action to the model's
   armature applies the difference twice. The visible result is a character
   standing in a pose it never had — arms out — that still breathes correctly,
   because the motion is right and only the base it rides on is wrong. The clips
   are therefore retargeted through armature space, which neither rest pose
   affects.

2. The FBX materials import with Alpha 0, which exports as alphaMode MASK with a
   zero baseColorFactor alpha. Such a mesh is cut out of the colour pass but
   still drawn into the shadow map: on screen the character is missing while its
   shadow walks around.
"""
import bpy
import os
import sys
from mathutils import Matrix

argv = sys.argv[sys.argv.index("--") + 1:]
model_fbx, anim_dir, skin_png, out_glb = argv

TARGET_HEIGHT = 1.8  # metres; Kenney's rig is roughly twice life size

bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    before = set(bpy.data.objects)
    # The rig and its animations must agree bone for bone, so the FBX's own bone
    # orientations are kept rather than re-derived from the hierarchy.
    bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False,
                             ignore_leaf_bones=False)
    return [o for o in bpy.data.objects if o not in before]


def depth(pose_bone):
    n = 0
    parent = pose_bone.parent
    while parent is not None:
        n += 1
        parent = parent.parent
    return n


# --- rig + mesh -------------------------------------------------------------
imported = import_fbx(model_fbx)
armature = next(o for o in imported if o.type == 'ARMATURE')
meshes = [o for o in imported if o.type == 'MESH']
armature.name = "Root"
rest_height = max((o.dimensions[2] for o in meshes), default=0.0)

# Parent-first: a bone's armature-space matrix is only meaningful once its
# parents are already placed.
ordered = sorted(armature.pose.bones, key=depth)

# --- skin -------------------------------------------------------------------
image = bpy.data.images.load(skin_png)
for mesh in meshes:
    for slot in mesh.material_slots:
        material = slot.material
        if material is None:
            continue
        material.use_nodes = True
        nodes, links = material.node_tree.nodes, material.node_tree.links
        bsdf = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
        texture = nodes.new('ShaderNodeTexImage')
        texture.image = image
        texture.interpolation = 'Closest'  # Kenney skins are flat colour atlases
        links.new(texture.outputs['Color'], bsdf.inputs['Base Color'])
        bsdf.inputs['Roughness'].default_value = 0.75
        bsdf.inputs['Metallic'].default_value = 0.0
        bsdf.inputs['Alpha'].default_value = 1.0
        material.blend_method = 'OPAQUE'

# --- animation clips --------------------------------------------------------
if not armature.animation_data:
    armature.animation_data_create()

scene = bpy.context.scene
view_layer = bpy.context.view_layer

for pose_bone in ordered:
    pose_bone.rotation_mode = 'QUATERNION'


def retarget(source_armature, source_action, clip_name):
    """Copies a pose frame by frame in armature space — the one space both rigs
    agree on despite their different rest poses."""
    if not source_armature.animation_data:
        source_armature.animation_data_create()
    source_armature.animation_data.action = source_action

    first, last = (int(round(v)) for v in source_action.frame_range)
    target = bpy.data.actions.new(clip_name)
    target.use_fake_user = True
    armature.animation_data.action = target

    for frame in range(first, last + 1):
        scene.frame_set(frame)
        for pose_bone in ordered:
            source = source_armature.pose.bones.get(pose_bone.name)
            if source is None:
                continue
            pose_bone.matrix = Matrix(source.matrix)
            # Children read their parent's placement, so it has to be real before
            # they are set rather than at the end of the frame.
            view_layer.update()
        for pose_bone in ordered:
            pose_bone.keyframe_insert("location", frame=frame)
            pose_bone.keyframe_insert("rotation_quaternion", frame=frame)
            pose_bone.keyframe_insert("scale", frame=frame)

    armature.animation_data.action = None
    track = armature.animation_data.nla_tracks.new()
    track.name = clip_name
    track.strips.new(clip_name, first, target)
    return clip_name


clips = []
aim_taken = False
for entry in sorted(os.listdir(anim_dir)):
    if not entry.lower().endswith(".fbx"):
        continue
    clip = os.path.splitext(entry)[0]
    known = set(bpy.data.actions)
    extra = import_fbx(os.path.join(anim_dir, entry))
    source_armature = next((o for o in extra if o.type == 'ARMATURE'), None)
    new_actions = [a for a in bpy.data.actions if a not in known]
    if source_armature is None or not new_actions:
        print("WARN no usable animation in", entry)
        continue

    # Every Kenney animation file also carries a 2-frame "Targeting Pose": the
    # real clip is the one that actually spans frames, and the short one makes a
    # serviceable aim pose, taken once.
    new_actions.sort(key=lambda a: a.frame_range[1] - a.frame_range[0])
    if len(new_actions) > 1 and not aim_taken:
        clips.append(retarget(source_armature, new_actions[0], "aim"))
        aim_taken = True
    clips.append(retarget(source_armature, new_actions[-1], clip))

    for obj in extra:
        bpy.data.objects.remove(obj, do_unlink=True)

# Normalising on the object keeps the clips untouched and lets the scene author a
# capsule in metres; the scale rides along in the exported glTF node.
factor = TARGET_HEIGHT / rest_height if rest_height > 0.0 else 1.0
armature.scale = (factor, factor, factor)

for obj in bpy.data.objects:
    obj.select_set(True)

bpy.ops.export_scene.gltf(
    filepath=out_glb,
    export_format='GLB',
    export_animations=True,
    export_animation_mode='NLA_TRACKS',
    export_nla_strips=True,
    export_apply=False,
    export_yup=True,
)

print("RESULT clips=%s bones=%d rest=%.2f scale=%.3f -> %s"
      % (clips, len(armature.data.bones), rest_height, factor, out_glb))
