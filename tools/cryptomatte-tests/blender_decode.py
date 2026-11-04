"""Decode a Cryptomatte layer of an EXR with Blender's compositor (an independent decoder), for
the encoder cross-check. Writes <outdir>/blender_matte_0001.exr (the Matte output).

    blender -b --python blender_decode.py -- <exr> <outdir> <layer name> <object name>
"""
import bpy, sys, os

argv = sys.argv[sys.argv.index("--") + 1:]
exr, outdir, layer, name = argv[0], argv[1], argv[2], argv[3]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
img = bpy.data.images.load(exr)
sc.render.resolution_x, sc.render.resolution_y = img.size[0], img.size[1]
sc.render.resolution_percentage = 100
sc.render.engine = 'BLENDER_WORKBENCH'
sc.frame_set(1)
# the compositor needs a camera to run a render
bpy.ops.object.camera_add(location=(0, 0, 10))
sc.camera = bpy.context.active_object

sc.use_nodes = True
tree = sc.node_tree
for n in list(tree.nodes):
    tree.nodes.remove(n)
crypto = tree.nodes.new("CompositorNodeCryptomatteV2")
crypto.source = 'IMAGE'
crypto.image = img
layers = [item.identifier for item in crypto.bl_rna.properties["layer_name"].enum_items]
print("BLENDER layers:", layers)
crypto.layer_name = layer
crypto.matte_id = name
out = tree.nodes.new("CompositorNodeOutputFile")
out.base_path = outdir
out.format.file_format = 'OPEN_EXR'
out.format.color_depth = '32'
out.format.exr_codec = 'ZIP'
out.file_slots[0].path = "blender_matte_"
tree.links.new(crypto.outputs["Matte"], out.inputs[0])
comp = tree.nodes.new("CompositorNodeComposite")
tree.links.new(crypto.outputs["Image"], comp.inputs[0])

bpy.ops.render.render(write_still=False)
print("BLENDER wrote", os.path.join(outdir, "blender_matte_0001.exr"), os.path.isfile(os.path.join(outdir, "blender_matte_0001.exr")))
sys.stdout.flush()
