"""
Blender Block-box Generator (F-2 + F-3)
Converts floorplan.json → 3D block-box meshes → FBX for UE5.

Usage:
  blender --background --python blender_blockbox.py -- --input floorplan.json --output out/ --mode all

Modes: building | bridge | all
"""

import sys
import os
import json
import math
import argparse

try:
    import bpy
    import bmesh
    from mathutils import Vector, Matrix
    BLENDER = True
except ImportError:
    BLENDER = False
    print("[blender_blockbox] Not running inside Blender.")


# ── Colors (RGB + alpha for Principled BSDF) ─────────────────────────────────

COLORS = {
    "exterior":   (0.65, 0.65, 0.70, 1.0),
    "interior":   (0.80, 0.75, 0.65, 1.0),
    "partition":  (0.85, 0.85, 0.80, 1.0),
    "floor":      (0.60, 0.60, 0.55, 1.0),
    "ceiling":    (0.70, 0.70, 0.68, 1.0),
    "stair":      (0.50, 0.70, 0.50, 1.0),
    "column":     (0.75, 0.55, 0.55, 1.0),
    "deck":       (0.55, 0.55, 0.60, 1.0),
    "pier":       (0.60, 0.55, 0.50, 1.0),
    "abutment":   (0.55, 0.50, 0.45, 1.0),
    "railing":    (0.70, 0.70, 0.70, 1.0),
    "sidewalk":   (0.62, 0.62, 0.65, 1.0),
}

_mat_cache = {}


def _get_material(key: str):
    if key not in _mat_cache:
        mat = bpy.data.materials.new(name=f"BB_{key}")
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            bsdf.inputs["Base Color"].default_value = COLORS.get(key, (0.7, 0.7, 0.7, 1.0))
            bsdf.inputs["Roughness"].default_value = 0.9
            bsdf.inputs["Specular IOR Level"].default_value = 0.05
        _mat_cache[key] = mat
    return _mat_cache[key]


def _assign_material(obj, key: str):
    mat = _get_material(key)
    if len(obj.data.materials) == 0:
        obj.data.materials.append(mat)
    else:
        obj.data.materials[0] = mat


# ── Scene Utilities (shared with blender_extrude.py) ──────────────────────────

def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for m in list(bpy.data.meshes):
        if m.users == 0:
            bpy.data.meshes.remove(m)


def set_units_metric():
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.length_unit = "METERS"


def export_fbx_ue5(objects: list, filepath: str):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    if objects:
        bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.fbx(
        filepath=filepath,
        use_selection=True,
        global_scale=1.0,
        apply_unit_scale=True,
        apply_scale_options="FBX_SCALE_NONE",
        axis_forward="-Z",
        axis_up="Y",
        use_mesh_modifiers=True,
        mesh_smooth_type="FACE",
        use_tspace=True,
        embed_textures=False,
        path_mode="COPY",
        bake_anim=False,
    )


# ── Wall Block ────────────────────────────────────────────────────────────────

def create_wall_block(wall: dict, elev: float, height: float) -> bpy.types.Object:
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    t = wall["thickness_m"]

    length = math.sqrt((ex - sx) ** 2 + (ey - sy) ** 2)
    if length < 0.01:
        return None
    angle = math.atan2(ey - sy, ex - sx)

    cx = (sx + ex) / 2.0
    cy = (sy + ey) / 2.0
    cz = elev + height / 2.0

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(cx, cy, cz))
    obj = bpy.context.active_object
    obj.scale = (length, t, height)
    obj.rotation_euler.z = angle

    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    obj.name = f"Wall_{wall['wall_id']}"
    _assign_material(obj, wall.get("type", "interior"))

    for opening in wall.get("openings", []):
        _apply_opening_boolean(obj, opening, wall, elev)

    _triangulate_object(obj)
    return obj


def _triangulate_object(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(obj.data)
    bm.free()


def _apply_opening_boolean(wall_obj, opening: dict, wall: dict, elev: float):
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    wall_len = math.sqrt((ex - sx) ** 2 + (ey - sy) ** 2)
    if wall_len < 0.01:
        return
    angle = math.atan2(ey - sy, ex - sx)
    dx, dy = (ex - sx) / wall_len, (ey - sy) / wall_len

    offset = opening["offset_m"]
    w = opening["width_m"]
    h = opening["height_m"]
    sill = opening.get("sill_m", 0.0)

    if offset + w > wall_len + 0.01:
        offset = max(0, wall_len - w)

    center_along = offset + w / 2.0
    ox = sx + dx * center_along
    oy = sy + dy * center_along
    oz = elev + sill + h / 2.0

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(ox, oy, oz))
    cutter = bpy.context.active_object
    cutter.scale = (w, wall["thickness_m"] * 2, h)
    cutter.rotation_euler.z = angle
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    cutter.name = f"Cutter_{opening.get('opening_id', 'op')}"

    try:
        mod = wall_obj.modifiers.new(name="Opening", type="BOOLEAN")
        mod.operation = "DIFFERENCE"
        mod.object = cutter
        bpy.context.view_layer.objects.active = wall_obj
        bpy.ops.object.modifier_apply(modifier=mod.name)
    except Exception as e:
        print(f"  Warning: Boolean failed for {opening.get('opening_id')}: {e}")
    finally:
        bpy.data.objects.remove(cutter, do_unlink=True)


# ── Floor / Ceiling Plate ─────────────────────────────────────────────────────

def create_floor_plate(rooms: list, elevation: float, thickness: float = 0.2,
                       label: str = "FloorPlate", mat_key: str = "floor"):
    if not rooms:
        return None

    plates = []
    for i, room in enumerate(rooms):
        poly = room["polygon"]
        if len(poly) < 3:
            continue

        bm = bmesh.new()
        bot = [bm.verts.new((x, y, elevation)) for x, y in poly]
        top = [bm.verts.new((x, y, elevation + thickness)) for x, y in poly]
        bm.verts.ensure_lookup_table()

        n = len(poly)
        bm.faces.new(top)
        bm.faces.new(list(reversed(bot)))
        for j in range(n):
            k = (j + 1) % n
            bm.faces.new([bot[j], bot[k], top[k], top[j]])

        bmesh.ops.triangulate(bm, faces=bm.faces[:])
        mesh = bpy.data.meshes.new(f"Plate_{i}")
        bm.to_mesh(mesh)
        bm.free()

        obj = bpy.data.objects.new(f"Plate_{i}", mesh)
        bpy.context.collection.objects.link(obj)
        plates.append(obj)

    if not plates:
        return None

    if len(plates) == 1:
        plates[0].name = label
        _assign_material(plates[0], mat_key)
        _triangulate_object(plates[0])
        return plates[0]

    base = plates[0]
    bpy.context.view_layer.objects.active = base
    for other in plates[1:]:
        mod = base.modifiers.new(name="Union", type="BOOLEAN")
        mod.operation = "UNION"
        mod.object = other
        try:
            bpy.ops.object.modifier_apply(modifier=mod.name)
        except Exception:
            pass
        bpy.data.objects.remove(other, do_unlink=True)

    base.name = label
    _assign_material(base, mat_key)
    _triangulate_object(base)
    return base


# ── Stair Block ───────────────────────────────────────────────────────────────

def create_stair_block(stair: dict, elev: float, height: float) -> bpy.types.Object:
    loc = stair["location"]
    sw, sd = stair["size_m"]
    stype = stair.get("type", "straight")

    if stype == "straight":
        bpy.ops.mesh.primitive_cube_add(size=1.0,
                                        location=(loc[0], loc[1], elev + height / 2.0))
        obj = bpy.context.active_object
        obj.scale = (sw, sd, height)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    elif stype == "180_turn":
        landing_d = sw
        run_d = (sd - landing_d) / 2.0
        objects = []

        bpy.ops.mesh.primitive_cube_add(size=1.0,
            location=(loc[0], loc[1] - sd / 2 + run_d / 2, elev + height / 4.0))
        r1 = bpy.context.active_object
        r1.scale = (sw, run_d, height / 2.0)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        objects.append(r1)

        bpy.ops.mesh.primitive_cube_add(size=1.0,
            location=(loc[0], loc[1], elev + height / 2.0))
        land = bpy.context.active_object
        land.scale = (sw, landing_d, 0.2)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        objects.append(land)

        bpy.ops.mesh.primitive_cube_add(size=1.0,
            location=(loc[0], loc[1] + sd / 2 - run_d / 2, elev + height * 3.0 / 4.0))
        r2 = bpy.context.active_object
        r2.scale = (sw, run_d, height / 2.0)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        objects.append(r2)

        bpy.context.view_layer.objects.active = r1
        for o in objects[1:]:
            mod = r1.modifiers.new(name="J", type="BOOLEAN")
            mod.operation = "UNION"
            mod.object = o
            try:
                bpy.ops.object.modifier_apply(modifier=mod.name)
            except Exception:
                pass
            bpy.data.objects.remove(o, do_unlink=True)
        obj = r1

    else:
        bpy.ops.mesh.primitive_cube_add(size=1.0,
                                        location=(loc[0], loc[1], elev + height / 2.0))
        obj = bpy.context.active_object
        obj.scale = (sw, sd, height)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    obj.name = f"Stair_{stair['stair_id']}"
    _assign_material(obj, "stair")
    _triangulate_object(obj)
    return obj


# ── Column Block ──────────────────────────────────────────────────────────────

def create_column_block(col: dict, elev: float, height: float) -> bpy.types.Object:
    cx, cy = col["center"]
    s = col["size_m"]
    sx = s[0] if isinstance(s, list) else s
    sy = s[1] if isinstance(s, list) and len(s) > 1 else sx
    shape = col.get("shape", "square")

    if shape == "circle":
        bpy.ops.mesh.primitive_cylinder_add(
            vertices=8, radius=sx / 2.0, depth=height,
            location=(cx, cy, elev + height / 2.0))
    else:
        bpy.ops.mesh.primitive_cube_add(
            size=1.0, location=(cx, cy, elev + height / 2.0))
        bpy.context.active_object.scale = (sx, sy, height)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    obj = bpy.context.active_object
    obj.name = f"Col_{col['column_id']}"
    _assign_material(obj, "column")
    _triangulate_object(obj)
    return obj


# ── Building Generator ────────────────────────────────────────────────────────

def generate_building(data: dict) -> list:
    floors = data.get("floors", [])
    if not floors:
        return []

    all_objects = []
    name = data.get("building_name", "Building")
    print(f"\n  Building: {name} ({len(floors)} floors)")

    for floor in floors:
        elev = floor["elevation_m"]
        h = floor["height_m"]
        fl = floor.get("label", f"F{floor['floor_index']}")
        print(f"    {fl}: elev={elev}m, h={h}m, "
              f"walls={len(floor.get('walls', []))}, "
              f"rooms={len(floor.get('rooms', []))}")

        fp = create_floor_plate(floor.get("rooms", []), elev, 0.2,
                                f"{name}_{fl}_Floor", "floor")
        if fp:
            all_objects.append(fp)

        cp = create_floor_plate(floor.get("rooms", []), elev + h - 0.15, 0.15,
                                f"{name}_{fl}_Ceiling", "ceiling")
        if cp:
            all_objects.append(cp)

        for wall in floor.get("walls", []):
            obj = create_wall_block(wall, elev, h)
            if obj:
                all_objects.append(obj)

        for stair in floor.get("stairs", []):
            obj = create_stair_block(stair, elev, h)
            if obj:
                all_objects.append(obj)

        for col in floor.get("columns", []):
            obj = create_column_block(col, elev, h)
            if obj:
                all_objects.append(obj)

    print(f"    Total objects: {len(all_objects)}")
    return all_objects


# ── Bridge Generator ──────────────────────────────────────────────────────────

def _vec2(a, b):
    return math.sqrt((b[0]-a[0])**2 + (b[1]-a[1])**2)


def generate_bridge(bridge: dict) -> list:
    path = bridge["path"]
    deck = bridge["deck"]
    bid = bridge.get("bridge_id", "bridge")
    w = deck["width_m"]
    thick = deck["thickness_m"]
    sw_w = deck.get("sidewalk_width_m", 0)
    sw_h = deck.get("sidewalk_height_offset_m", 0)

    all_objects = []
    print(f"\n  Bridge: {bid} (path points={len(path)})")

    for i in range(len(path) - 1):
        p0, p1 = path[i], path[i + 1]
        seg_len = _vec2(p0, p1)
        if seg_len < 0.01:
            continue
        dx = (p1[0] - p0[0]) / seg_len
        dy = (p1[1] - p0[1]) / seg_len
        nx, ny = -dy, dx
        mx = (p0[0] + p1[0]) / 2.0
        my = (p0[1] + p1[1]) / 2.0
        mz = (p0[2] + p1[2]) / 2.0
        angle = math.atan2(dy, dx)

        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(mx, my, mz - thick / 2.0))
        obj = bpy.context.active_object
        obj.scale = (seg_len, w, thick)
        obj.rotation_euler.z = angle
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        obj.name = f"{bid}_Deck_{i}"
        _assign_material(obj, "deck")
        all_objects.append(obj)

        if sw_w > 0:
            for side in [1, -1]:
                off_x = nx * side * (w / 2.0 - sw_w / 2.0)
                off_y = ny * side * (w / 2.0 - sw_w / 2.0)
                bpy.ops.mesh.primitive_cube_add(size=1.0,
                    location=(mx + off_x, my + off_y, mz + sw_h / 2.0))
                sw_obj = bpy.context.active_object
                sw_obj.scale = (seg_len, sw_w, sw_h + 0.05)
                sw_obj.rotation_euler.z = angle
                bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
                sw_obj.name = f"{bid}_SW_{i}_{'L' if side > 0 else 'R'}"
                _assign_material(sw_obj, "sidewalk")
                all_objects.append(sw_obj)

    total_len = sum(_vec2(path[i], path[i+1]) for i in range(len(path)-1))

    def _pos_at_station(station):
        cum = 0
        for i in range(len(path) - 1):
            sl = _vec2(path[i], path[i + 1])
            if cum + sl >= station:
                t = (station - cum) / sl if sl > 0 else 0
                return [path[i][j] + (path[i+1][j] - path[i][j]) * t for j in range(3)]
            cum += sl
        return list(path[-1])

    for pier in bridge.get("piers", []):
        pos = _pos_at_station(pier["station_m"])
        pw, pd = pier["width_m"], pier["depth_m"]
        ph = pier["top_z"] - pier["bottom_z"]
        pz = (pier["top_z"] + pier["bottom_z"]) / 2.0
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(pos[0], pos[1], pz))
        obj = bpy.context.active_object
        obj.scale = (pw, pd, ph)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        obj.name = f"{bid}_Pier_{pier.get('pier_id', '')}"
        _assign_material(obj, "pier")
        all_objects.append(obj)

    for abut in bridge.get("abutments", []):
        pos = _pos_at_station(abut["station_m"])
        aw, ad = abut["width_m"], abut["depth_m"]
        ah = abut["top_z"] - abut["bottom_z"]
        az = (abut["top_z"] + abut["bottom_z"]) / 2.0
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(pos[0], pos[1], az))
        obj = bpy.context.active_object
        obj.scale = (aw, ad, ah)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        obj.name = f"{bid}_Abut_{abut.get('abutment_id', '')}"
        _assign_material(obj, "abutment")
        all_objects.append(obj)

    railings = bridge.get("railings", {})
    rh = railings.get("height_m", 1.1)
    rt = railings.get("thickness_m", 0.15)
    if rh > 0:
        for i in range(len(path) - 1):
            p0, p1 = path[i], path[i + 1]
            seg_len = _vec2(p0, p1)
            if seg_len < 0.01:
                continue
            dx = (p1[0] - p0[0]) / seg_len
            dy = (p1[1] - p0[1]) / seg_len
            nx, ny = -dy, dx
            mx = (p0[0] + p1[0]) / 2.0
            my = (p0[1] + p1[1]) / 2.0
            mz = (p0[2] + p1[2]) / 2.0
            angle = math.atan2(dy, dx)

            for side in [1, -1]:
                off_x = nx * side * w / 2.0
                off_y = ny * side * w / 2.0
                bpy.ops.mesh.primitive_cube_add(size=1.0,
                    location=(mx + off_x, my + off_y, mz + rh / 2.0))
                obj = bpy.context.active_object
                obj.scale = (seg_len, rt, rh)
                obj.rotation_euler.z = angle
                bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
                obj.name = f"{bid}_Rail_{i}_{'L' if side > 0 else 'R'}"
                _assign_material(obj, "railing")
                all_objects.append(obj)

    print(f"    Total bridge objects: {len(all_objects)}")
    return all_objects


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    if not BLENDER:
        print("ERROR: Run inside Blender.")
        print("  blender --background --python blender_blockbox.py -- "
              "--input floorplan.json --output out/ --mode all")
        return

    argv = sys.argv
    if "--" not in argv:
        print("ERROR: Use '--' separator before arguments")
        return

    custom_args = argv[argv.index("--") + 1:]
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", "-i", required=True)
    parser.add_argument("--output", "-o", required=True)
    parser.add_argument("--mode", "-m", default="all", choices=["building", "bridge", "all"])
    parser.add_argument("--combine", action="store_true",
                        help="Join all objects into a single mesh for easy UE5 drag-and-drop")
    args = parser.parse_args(custom_args)

    if not os.path.exists(args.input):
        print(f"ERROR: File not found: {args.input}")
        return

    with open(args.input, encoding="utf-8") as f:
        data = json.load(f)

    os.makedirs(args.output, exist_ok=True)

    print(f"\n{'='*50}")
    print(f"  Blender Block-box Generator")
    print(f"  Input : {args.input}")
    print(f"  Output: {args.output}")
    print(f"  Mode  : {args.mode}")
    print(f"{'='*50}")

    clear_scene()
    set_units_metric()

    all_objects = []

    if args.mode in ("building", "all") and data.get("floors"):
        bldg_objs = generate_building(data)
        all_objects.extend(bldg_objs)

        if bldg_objs:
            name = data.get("building_name", "building")
            fbx_path = os.path.join(args.output, f"{name}.fbx")
            export_fbx_ue5(bldg_objs, fbx_path)
            print(f"\n  Exported building: {fbx_path}")

    if args.mode in ("bridge", "all") and data.get("bridges"):
        for bridge in data["bridges"]:
            br_objs = generate_bridge(bridge)
            all_objects.extend(br_objs)

            if br_objs:
                bid = bridge.get("bridge_id", "bridge")
                fbx_path = os.path.join(args.output, f"{bid}.fbx")
                export_fbx_ue5(br_objs, fbx_path)
                print(f"  Exported bridge: {fbx_path}")

    if all_objects and args.mode == "all" and not args.combine:
        combined = os.path.join(args.output, "combined.fbx")
        export_fbx_ue5(all_objects, combined)
        print(f"  Exported combined: {combined}")

    if args.combine and all_objects:
        joined = _join_objects(all_objects, data.get("building_name", "building"))
        name = data.get("building_name", "building")
        fbx_path = os.path.join(args.output, f"{name}_combined.fbx")
        export_fbx_ue5([joined], fbx_path)
        print(f"\n  Exported COMBINED single mesh: {fbx_path}")

    print(f"\n  Total objects: {len(all_objects)}")
    print(f"{'='*50}\n")


def _join_objects(objects: list, name: str = "Combined"):
    """Join all objects into a single mesh object."""
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:
        if obj and obj.name in bpy.data.objects:
            obj.select_set(True)
    if not objects:
        return None
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    joined = bpy.context.active_object
    joined.name = name
    _triangulate_object(joined)
    return joined


if __name__ == "__main__":
    main()
