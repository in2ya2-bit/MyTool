"""
Blender Building Extrude Script
Run inside Blender: blender --background --python blender_extrude.py -- <buildings.json>

Usage:
  blender --background --python blender_extrude.py -- output/buildings/Seoul_Jongno_buildings.json

Output:
  - output/buildings/fbx/<building_id>.fbx   per building mesh
  - output/buildings/buildings_combined.fbx  all buildings merged
"""

import sys
import os
import json
import math

# ─── Blender Import Guard ─────────────────────────────────────────────────────
try:
    import bpy
    import bmesh
    from mathutils import Vector, Matrix
    BLENDER = True
except ImportError:
    BLENDER = False
    print("[blender_extrude] Not running inside Blender — script is for Blender execution only.")


# ─── Constants ────────────────────────────────────────────────────────────────

# UE5 FBX export settings
UE5_SCALE      = 0.01    # Blender 1m → UE5 100cm (scale on import = 1.0)
WALL_UV_SCALE  = 1.0 / 4.0   # 1 UV tile per 4 meters of wall
ROOF_UV_SCALE  = 1.0 / 8.0


# ─── Blender Scene Utilities ──────────────────────────────────────────────────

def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def set_units_metric():
    scene = bpy.context.scene
    scene.unit_settings.system      = "METRIC"
    scene.unit_settings.length_unit = "METERS"


# ─── Material Creation ────────────────────────────────────────────────────────

MATERIAL_CACHE = {}

BUILDING_COLORS = {
    "BP_Building_Residential":  (0.72, 0.60, 0.50, 1.0),
    "BP_Building_Apartment":    (0.65, 0.65, 0.70, 1.0),
    "BP_Building_Commercial":   (0.50, 0.65, 0.80, 1.0),
    "BP_Building_Industrial":   (0.55, 0.55, 0.50, 1.0),
    "BP_Building_Office":       (0.60, 0.70, 0.75, 1.0),
    "BP_Building_Retail":       (0.80, 0.65, 0.55, 1.0),
    "BP_Building_Warehouse":    (0.60, 0.58, 0.52, 1.0),
    "BP_Building_Church":       (0.85, 0.82, 0.78, 1.0),
    "BP_Building_School":       (0.78, 0.75, 0.60, 1.0),
    "BP_Building_Hospital":     (0.92, 0.92, 0.90, 1.0),
    "BP_Building_Generic":      (0.70, 0.65, 0.62, 1.0),
}

def get_material(building_type: str):
    if building_type not in MATERIAL_CACHE:
        mat = bpy.data.materials.new(name=building_type)
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            color = BUILDING_COLORS.get(building_type, (0.7, 0.65, 0.62, 1.0))
            bsdf.inputs["Base Color"].default_value = color
            bsdf.inputs["Roughness"].default_value  = 0.85
            bsdf.inputs["Specular"].default_value   = 0.1
        MATERIAL_CACHE[building_type] = mat
    return MATERIAL_CACHE[building_type]


# ─── Mesh Generation ─────────────────────────────────────────────────────────

def create_building_mesh(building: dict) -> bpy.types.Object:
    """
    Create a 3D building mesh from a footprint polygon.
    Applies:
      - Extruded walls from footprint
      - Flat roof
      - UV unwrap (box projection)
      - Material assignment by type
    """
    footprint_ue5 = building["footprint_ue5"]
    height_m      = building["height_m"]
    cx, cy        = building["centroid_ue5"]
    btype         = building["type"]
    bid           = building["id"]

    # Convert UE5 cm coordinates → Blender meters, centered at origin
    verts_2d = [
        ((x - cx) * 0.01, (y - cy) * 0.01)   # cm → m, center
        for x, y in footprint_ue5
    ]

    # Remove duplicate closing vertex if present
    if len(verts_2d) > 1 and verts_2d[0] == verts_2d[-1]:
        verts_2d = verts_2d[:-1]

    if len(verts_2d) < 3:
        return None

    # ── Build mesh with BMesh ──────────────────────────────────────────────
    mesh = bpy.data.meshes.new(f"Building_{bid}")
    bm   = bmesh.new()

    n = len(verts_2d)

    # Bottom ring
    bottom_verts = [bm.verts.new((x, y, 0.0)) for x, y in verts_2d]
    # Top ring
    top_verts    = [bm.verts.new((x, y, height_m)) for x, y in verts_2d]

    bm.verts.ensure_lookup_table()

    # Wall faces (quads between bottom and top rings)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new([
            bottom_verts[i],
            bottom_verts[j],
            top_verts[j],
            top_verts[i],
        ])

    # Roof face
    bm.faces.new(top_verts)

    # Bottom cap (hidden, helps with collision)
    bm.faces.new(list(reversed(bottom_verts)))

    bm.normal_update()

    # ── UV Unwrap ────────────────────────────────────────────────────────
    uv_layer = bm.loops.layers.uv.new("UVMap")

    for face in bm.faces:
        if abs(face.normal.z) > 0.8:
            # Roof / floor — planar UV
            for loop in face.loops:
                u = loop.vert.co.x * ROOF_UV_SCALE
                v = loop.vert.co.y * ROOF_UV_SCALE
                loop[uv_layer].uv = (u, v)
        else:
            # Wall — use edge length for U, height for V
            for i, loop in enumerate(face.loops):
                if i == 0:
                    u, v = 0.0, loop.vert.co.z * WALL_UV_SCALE
                elif i == 1:
                    edge_len = (face.loops[0].vert.co - loop.vert.co).length
                    u = edge_len * WALL_UV_SCALE
                    v = loop.vert.co.z * WALL_UV_SCALE
                elif i == 2:
                    edge_len = (face.loops[0].vert.co - face.loops[1].vert.co).length
                    u = edge_len * WALL_UV_SCALE
                    v = loop.vert.co.z * WALL_UV_SCALE
                else:
                    u = 0.0
                    v = loop.vert.co.z * WALL_UV_SCALE
                loop[uv_layer].uv = (u, v)

    bm.to_mesh(mesh)
    bm.free()

    # Create object
    obj = bpy.data.objects.new(f"Building_{bid}", mesh)
    obj["building_type"] = btype
    obj["building_id"]   = bid
    obj["height_m"]      = height_m
    obj["centroid_x"]    = cx
    obj["centroid_y"]    = cy

    # Move to world position
    obj.location = (cx * 0.01, cy * 0.01, 0.0)   # cm → m

    bpy.context.collection.objects.link(obj)

    # Assign material
    mat = get_material(btype)
    if len(obj.data.materials) == 0:
        obj.data.materials.append(mat)
    else:
        obj.data.materials[0] = mat

    return obj


# ─── LOD Generation ──────────────────────────────────────────────────────────

def add_lod_decimate(obj: bpy.types.Object, ratio: float = 0.5):
    """Add Decimate modifier for LOD generation."""
    mod = obj.modifiers.new(name="LOD_Decimate", type="DECIMATE")
    mod.ratio       = ratio
    mod.use_collapse_triangulate = True


# ─── FBX Export ──────────────────────────────────────────────────────────────

def export_fbx_ue5(objects: list, filepath: str):
    """
    Export objects as FBX with UE5-compatible settings.
    - Forward: -Z, Up: Y (UE5 standard)
    - Scale: 1.0 (already in cm via object scale)
    """
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0] if objects else None

    bpy.ops.export_scene.fbx(
        filepath               = filepath,
        use_selection          = True,
        global_scale           = 1.0,
        apply_unit_scale       = True,
        apply_scale_options    = "FBX_SCALE_NONE",
        axis_forward           = "-Z",
        axis_up                = "Y",
        use_mesh_modifiers     = True,
        mesh_smooth_type       = "FACE",
        use_tspace             = True,
        embed_textures         = False,
        path_mode              = "COPY",
        bake_anim              = False,
    )


# ─── Main Entry Point ─────────────────────────────────────────────────────────

def main():
    if not BLENDER:
        print("ERROR: This script must be run inside Blender.")
        print("Usage: blender --background --python blender_extrude.py -- <buildings.json>")
        return

    # Parse arguments after '--'
    argv = sys.argv
    if "--" in argv:
        args = argv[argv.index("--") + 1:]
    else:
        print("ERROR: Provide buildings JSON path after '--'")
        return

    if not args:
        print("ERROR: buildings.json path required")
        return

    json_path = args[0]
    if not os.path.exists(json_path):
        print(f"ERROR: File not found: {json_path}")
        return

    with open(json_path) as f:
        buildings = json.load(f)

    out_dir = os.path.join(os.path.dirname(json_path), "fbx")
    os.makedirs(out_dir, exist_ok=True)

    print(f"\n=== Blender Building Extrude ===")
    print(f"  Input : {json_path}")
    print(f"  Count : {len(buildings)} buildings")
    print(f"  Output: {out_dir}\n")

    # Setup
    clear_scene()
    set_units_metric()

    all_objects = []
    skipped     = 0

    for i, bldg in enumerate(buildings):
        obj = create_building_mesh(bldg)
        if obj is None:
            skipped += 1
            continue
        all_objects.append(obj)

        # Add LOD decimate for complex footprints (> 8 verts)
        if len(bldg["footprint_ue5"]) > 8:
            add_lod_decimate(obj, ratio=0.6)

        if (i + 1) % 100 == 0:
            print(f"  Generated {i+1}/{len(buildings)} buildings...")

    print(f"\n  Created: {len(all_objects)} objects, Skipped: {skipped}")

    # Export combined FBX
    combined_path = os.path.join(out_dir, "buildings_combined.fbx")
    export_fbx_ue5(all_objects, combined_path)
    print(f"  Combined FBX: {combined_path}")

    # Export per-type FBX groups
    type_groups: dict = {}
    for obj in all_objects:
        btype = obj.get("building_type", "BP_Building_Generic")
        type_groups.setdefault(btype, []).append(obj)

    for btype, objs in type_groups.items():
        type_path = os.path.join(out_dir, f"{btype}.fbx")
        export_fbx_ue5(objs, type_path)
        print(f"  Exported {len(objs):4d} × {btype}")

    print(f"\n=== Done ===\n")


if __name__ == "__main__":
    main()
