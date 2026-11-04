import bpy, os, sys
OUT = sys.argv[sys.argv.index("--")+1]
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.engine = 'CYCLES'
sc.cycles.samples = 16
sc.cycles.device = 'CPU'
sc.render.film_transparent = True   # alpha = object coverage, so the all-objects matte must equal it
sc.render.resolution_x = 320; sc.render.resolution_y = 240; sc.render.resolution_percentage = 100
sc.frame_set(1)
def mat(name, col):
    m = bpy.data.materials.new(name); m.use_nodes = True
    m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (*col, 1); return m
mats = {"RedMat": mat("RedMat", (0.8, 0.1, 0.1)), "GreenMat": mat("GreenMat", (0.1, 0.8, 0.1)), "BlueMat": mat("BlueMat", (0.1, 0.1, 0.8))}
def add(kind, name, loc, m, asset=None):
    getattr(bpy.ops.mesh, "primitive_%s_add" % kind)(location=loc)
    o = bpy.context.active_object; o.name = name; o.data.materials.append(m)
    if asset:
        o.parent = asset
    return o
bpy.ops.object.empty_add(location=(0,0,0)); grp = bpy.context.active_object; grp.name = "HeroGroup"
add("cube", "Cube_Left", (-2.2, 0, 0), mats["RedMat"], grp)
add("uv_sphere", "Sphere_Mid", (0, 0, 0), mats["GreenMat"], grp)
add("monkey", "Suzanne_Right", (2.2, 0, 0), mats["BlueMat"])
add("plane", "Ground", (0, 0, -1.2), mats["GreenMat"]); bpy.context.active_object.scale = (6, 6, 1)
bpy.ops.object.light_add(type='SUN', location=(3, -3, 6)); bpy.context.active_object.data.energy = 4
bpy.ops.object.camera_add(location=(0, -9, 2.2), rotation=(1.35, 0, 0)); sc.camera = bpy.context.active_object
vl = sc.view_layers[0]
vl.use_pass_cryptomatte_object = True
vl.use_pass_cryptomatte_material = True
vl.use_pass_cryptomatte_asset = True
vl.pass_cryptomatte_depth = 6
vl.use_pass_z = True
sc.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
sc.render.image_settings.color_depth = '32'
sc.render.image_settings.exr_codec = 'ZIP'
sc.render.filepath = OUT
bpy.ops.render.render(write_still=True)
print("WROTE", OUT); sys.stdout.flush()
