"""Writes ref/parts.gltf, a four-triangle glTF for the Cryptomatte naming checks in
run_encode_check.sh: nodes PartA (material MatA) and PartB (MatB), plus two nodes both named
Dup under parents L (MatA) and R (MatB), so the manifest must show <node>/PartA, <node>/PartB,
<node>/L/Dup and <node>/R/Dup and the material layer MatA / MatB. The triangles sit in a row
in front of the encode_check scene's objects (y -0.3 .. 0.25, z 1.6, x -1.5 .. 1.5), inside the view of
its 50 mm camera at z 6.

    python make_parts_gltf.py            (from this folder; the file is tracked, re-run only to change it)
"""
import base64, json, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "ref", "parts.gltf")

centres = [-1.2, -0.4, 0.4, 1.2]
blob = bytearray()
accessors, views, meshes, nodes = [], [], [], []
materials = [
    {"name": "MatA", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.2, 0.2, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.6}},
    {"name": "MatB", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.3, 0.9, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.6}},
]
# (node name, parent name or None, material index)
parts = [("PartA", None, 0), ("PartB", None, 1), ("Dup", "L", 0), ("Dup", "R", 1)]

def add_view(data, target):
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
    blob.extend(data)
    return len(views) - 1

for i, (name, parent, mat) in enumerate(parts):
    cx = centres[i]
    pos = [(cx - 0.3, -0.3, 1.6), (cx + 0.3, -0.3, 1.6), (cx, 0.25, 1.6)]   # CCW seen from +Z
    pdata = b"".join(struct.pack("<3f", *p) for p in pos)
    idata = struct.pack("<3H", 0, 1, 2)
    pv = add_view(pdata, 34962)
    iv = add_view(idata, 34963)
    accessors.append({"bufferView": pv, "componentType": 5126, "count": 3, "type": "VEC3",
                      "min": [min(p[k] for p in pos) for k in range(3)], "max": [max(p[k] for p in pos) for k in range(3)]})
    accessors.append({"bufferView": iv, "componentType": 5123, "count": 3, "type": "SCALAR"})
    meshes.append({"name": name + "_mesh", "primitives": [{"attributes": {"POSITION": len(accessors) - 2},
                                                           "indices": len(accessors) - 1, "material": mat}]})
    nodes.append({"name": name, "mesh": len(meshes) - 1})

root_children = []
for i, (name, parent, mat) in enumerate(parts):
    if parent is None:
        root_children.append(i)
    else:
        nodes.append({"name": parent, "children": [i]})
        root_children.append(len(nodes) - 1)
nodes.append({"name": "parts", "children": root_children})

gltf = {
    "asset": {"version": "2.0", "generator": "Natron tools/cryptomatte-tests/make_parts_gltf.py"},
    "scene": 0,
    "scenes": [{"nodes": [len(nodes) - 1]}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": accessors,
    "bufferViews": views,
    "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii")}],
}
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", newline="\n") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", OUT, len(blob), "bytes of geometry")
