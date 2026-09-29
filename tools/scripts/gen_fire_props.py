"""Generates the campfire and torch props (glTF 2.0 + .bin): boxes only, flat
normals, one material per part (baseColorFactor, no textures)."""
import json, math, struct, os

def rot_y(p, a):
    c, s = math.cos(a), math.sin(a)
    return (c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2])

def rot_x(p, a):
    c, s = math.cos(a), math.sin(a)
    return (p[0], c * p[1] - s * p[2], s * p[1] + c * p[2])

def rot_z(p, a):
    c, s = math.cos(a), math.sin(a)
    return (c * p[0] - s * p[1], s * p[0] + c * p[1], p[2])

def box(size, center, yaw=0.0, pitch=0.0, roll=0.0):
    """Returns (positions, normals, uvs, indices) for an oriented box."""
    sx, sy, sz = size[0] / 2, size[1] / 2, size[2] / 2
    faces = [  # normal, four corners (ccw seen from outside)
        ((0, 0, 1), [(-sx, -sy, sz), (sx, -sy, sz), (sx, sy, sz), (-sx, sy, sz)]),
        ((0, 0, -1), [(sx, -sy, -sz), (-sx, -sy, -sz), (-sx, sy, -sz), (sx, sy, -sz)]),
        ((1, 0, 0), [(sx, -sy, sz), (sx, -sy, -sz), (sx, sy, -sz), (sx, sy, sz)]),
        ((-1, 0, 0), [(-sx, -sy, -sz), (-sx, -sy, sz), (-sx, sy, sz), (-sx, sy, -sz)]),
        ((0, 1, 0), [(-sx, sy, sz), (sx, sy, sz), (sx, sy, -sz), (-sx, sy, -sz)]),
        ((0, -1, 0), [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, -sy, sz), (-sx, -sy, sz)]),
    ]
    P, N, T, I = [], [], [], []
    def xf(p):
        p = rot_z(p, roll)
        p = rot_x(p, pitch)
        p = rot_y(p, yaw)
        return (p[0] + center[0], p[1] + center[1], p[2] + center[2])
    def xn(n):
        n = rot_z(n, roll)
        n = rot_x(n, pitch)
        return rot_y(n, yaw)
    for n, corners in faces:
        base = len(P)
        nn = xn(n)
        for k, c in enumerate(corners):
            P.append(xf(c))
            N.append(nn)
            T.append([(0, 0), (1, 0), (1, 1), (0, 1)][k])
        I += [base, base + 1, base + 2, base, base + 2, base + 3]
    return P, N, T, I

def write_gltf(path, parts, name):
    """parts: list of (material_name, color(rgb), [boxes]) with boxes = list of box() results."""
    blob = bytearray()
    views, accessors, prims, materials = [], [], [], []
    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        return len(views) - 1
    for mi, (mname, color, boxes) in enumerate(parts):
        P, N, T, I = [], [], [], []
        for (p, n, t, i) in boxes:
            base = len(P)
            P += p; N += n; T += t; I += [base + k for k in i]
        pos = b''.join(struct.pack('<3f', *v) for v in P)
        nor = b''.join(struct.pack('<3f', *v) for v in N)
        uv = b''.join(struct.pack('<2f', *v) for v in T)
        idx = b''.join(struct.pack('<H', k) for k in I)
        vp = add_view(pos, 34962); vn = add_view(nor, 34962); vt = add_view(uv, 34962); vi = add_view(idx, 34963)
        mn = [min(v[k] for v in P) for k in range(3)]
        mx = [max(v[k] for v in P) for k in range(3)]
        a0 = len(accessors)
        accessors.append({"bufferView": vp, "componentType": 5126, "count": len(P), "type": "VEC3", "min": mn, "max": mx})
        accessors.append({"bufferView": vn, "componentType": 5126, "count": len(N), "type": "VEC3"})
        accessors.append({"bufferView": vt, "componentType": 5126, "count": len(T), "type": "VEC2"})
        accessors.append({"bufferView": vi, "componentType": 5123, "count": len(I), "type": "SCALAR"})
        materials.append({"name": mname, "doubleSided": False,
                          "pbrMetallicRoughness": {"baseColorFactor": [color[0], color[1], color[2], 1.0],
                                                   "metallicFactor": 0.0, "roughnessFactor": 0.9}})
        prims.append({"attributes": {"POSITION": a0, "NORMAL": a0 + 1, "TEXCOORD_0": a0 + 2}, "indices": a0 + 3, "material": mi})
    binname = os.path.splitext(os.path.basename(path))[0] + '.bin'
    doc = {
        "asset": {"generator": "meadows prop generator", "version": "2.0"},
        "scene": 0, "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "materials": materials,
        "meshes": [{"name": name, "primitives": prims}],
        "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": binname}],
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=1)
    with open(os.path.join(os.path.dirname(path), binname), 'wb') as f:
        f.write(bytes(blob))
    print(path, len(blob), 'bytes')

out = 'game/data/base/models/props'
os.makedirs(out, exist_ok=True)

# --- Campfire: a stone ring, three crossed logs, a bed of embers ---------
stones = []
for k in range(9):
    a = k * 2 * math.pi / 9 + 0.3 * math.sin(k * 3.1)
    r = 0.62
    sz = 0.16 + 0.05 * math.sin(k * 2.3)
    stones.append(box((sz * 1.3, sz, sz), (r * math.cos(a), sz * 0.45, r * math.sin(a)),
                      yaw=a + 0.4 * math.cos(k), pitch=0.15 * math.sin(k * 1.7)))
logs = []
for k in range(3):
    a = k * math.pi / 3 + 0.2
    logs.append(box((0.85, 0.12, 0.12), (0.0, 0.16 + 0.03 * k, 0.0), yaw=a, roll=0.35 * (1 if k % 2 else -1)))
logs.append(box((0.6, 0.1, 0.1), (0.1, 0.06, 0.05), yaw=1.1))
embers = [box((0.5, 0.06, 0.5), (0.0, 0.03, 0.0), yaw=0.4)]
write_gltf(os.path.join(out, 'campfire.gltf'),
           [("stone", (0.42, 0.40, 0.38), stones),
            ("log", (0.30, 0.19, 0.10), logs),
            ("embers", (0.55, 0.12, 0.03), embers)], "Campfire")

# --- Torch: a post, an iron band, a wrapped head ------------------------
post = [box((0.07, 1.5, 0.07), (0.0, 0.75, 0.0))]
band = [box((0.11, 0.05, 0.11), (0.0, 1.32, 0.0), yaw=0.2)]
head = [box((0.15, 0.22, 0.15), (0.0, 1.50, 0.0), yaw=0.4),
        box((0.13, 0.08, 0.13), (0.0, 1.64, 0.0), yaw=0.9)]
write_gltf(os.path.join(out, 'torch.gltf'),
           [("wood", (0.33, 0.22, 0.12), post),
            ("iron", (0.22, 0.22, 0.24), band),
            ("wrap", (0.16, 0.12, 0.09), head)], "Torch")
