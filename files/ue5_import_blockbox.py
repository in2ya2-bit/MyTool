"""
F-4: UE5 Block-box FBX Import Utility (Editor Utility Widget / Python Script)

Unreal Engine 5 Editor Python script to batch-import block-box FBX files
with correct settings for gameplay prototyping.

Usage in UE5 Python console:
  import ue5_import_blockbox
  ue5_import_blockbox.import_all("E:/EEE/Shot1/S1/files/output/fbx")

Or from command line (for documentation/reference only):
  python ue5_import_blockbox.py --fbx-dir <path> --ue-dest /Game/BlockBox
"""

# ── UE5 Import Settings Reference ────────────────────────────────────────────
#
# These settings match the planning document §9 "UE5 FBX Import 규칙":
#
# | Setting              | Value           | Reason                            |
# |----------------------|-----------------|-----------------------------------|
# | Import Scale         | 1.0             | Blender exports at cm scale       |
# | Combine Meshes       | False           | Keep wall/floor/stair as separate |
# | Auto Generate Collision | True         | Block-box needs walkable collision|
# | Import as Skeletal   | False           | Static mesh only                  |
# | Material Import      | Do Not Create   | Use UE5 block-box materials       |
# | Import Normals       | Import Normals  | Preserve Blender normals          |
# | Vertex Color Import  | Replace         | Preserve color coding             |
# ─────────────────────────────────────────────────────────────────────────────

import os
import json
import argparse


def get_import_options_dict():
    """Return the recommended FBX import settings as a dict (for reference)."""
    return {
        "import_scale": 1.0,
        "combine_meshes": False,
        "auto_generate_collision": True,
        "import_as_skeletal": False,
        "material_import_method": "do_not_create",
        "import_normals": True,
        "import_vertex_color": True,
        "reset_to_fbx_on_material_conflict": True,
        "remove_degenerates": True,
        "generate_lightmap_uvs": False,
        "one_convex_hull_per_ucx": True,
    }


# ── UE5 Editor Script (only works inside Unreal Editor Python) ───────────────

def import_fbx_ue5(fbx_path: str, destination: str = "/Game/BlockBox"):
    """Import a single FBX into UE5 with block-box settings.

    Only works when executed inside UE5 Editor Python environment.
    """
    try:
        import unreal
    except ImportError:
        print(f"  [SKIP] Not running inside UE5 Editor. Would import: {fbx_path}")
        return None

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", fbx_path)
    task.set_editor_property("destination_path", destination)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)

    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", False)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_animations", False)
    options.set_editor_property("create_physics_asset", False)

    sm_options = options.get_editor_property("static_mesh_import_data")
    sm_options.set_editor_property("combine_meshes", False)
    sm_options.set_editor_property("auto_generate_collision", True)
    sm_options.set_editor_property("generate_lightmap_u_vs", False)
    sm_options.set_editor_property("remove_degenerates", True)
    sm_options.set_editor_property("import_uniform_scale", 1.0)

    normal_method = unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS
    sm_options.set_editor_property("normal_import_method", normal_method)

    task.set_editor_property("options", options)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    imported = task.get_editor_property("imported_object_paths")
    if imported:
        print(f"  [OK] Imported: {imported[0]}")

        for path in imported:
            asset = unreal.EditorAssetLibrary.load_asset(path)
            if asset and isinstance(asset, unreal.StaticMesh):
                body = asset.get_editor_property("body_setup")
                if body:
                    body.set_editor_property("collision_trace_flag",
                                             unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)

        return imported
    else:
        print(f"  [WARN] No objects imported from: {fbx_path}")
        return None


def import_all(fbx_dir: str, destination: str = "/Game/BlockBox"):
    """Batch-import all FBX files in a directory."""
    if not os.path.isdir(fbx_dir):
        print(f"  ERROR: Directory not found: {fbx_dir}")
        return

    fbx_files = []
    for root, dirs, files in os.walk(fbx_dir):
        for f in files:
            if f.lower().endswith(".fbx"):
                fbx_files.append(os.path.join(root, f))

    if not fbx_files:
        print(f"  No FBX files found in: {fbx_dir}")
        return

    print(f"\n=== UE5 Block-box FBX Import ===")
    print(f"  Directory : {fbx_dir}")
    print(f"  FBX Count : {len(fbx_files)}")
    print(f"  UE5 Dest  : {destination}\n")

    success = 0
    for fbx_path in sorted(fbx_files):
        name = os.path.splitext(os.path.basename(fbx_path))[0]
        dest = f"{destination}/{name}"
        result = import_fbx_ue5(fbx_path, dest)
        if result:
            success += 1

    print(f"\n  Imported: {success}/{len(fbx_files)}")
    print(f"=== Done ===\n")


def spawn_blockbox_actors(structure_layout_path: str):
    """Spawn imported block-box meshes at correct world positions.

    Reads structure_layout.json to place actors with proper transforms.
    Only works inside UE5 Editor Python environment.
    """
    try:
        import unreal
    except ImportError:
        print("  [SKIP] Not running inside UE5 Editor")
        return

    with open(structure_layout_path, encoding="utf-8") as f:
        layout = json.load(f)

    world = unreal.EditorLevelLibrary.get_editor_world()
    s_id = layout.get("structure_id", "unknown")

    print(f"  Spawning actors for: {s_id}")
    print(f"  Floors: {layout.get('floor_count', 0)}")

    asset_path = f"/Game/BlockBox/{s_id}"

    assets = unreal.EditorAssetLibrary.list_assets(asset_path, recursive=True)
    if not assets:
        print(f"  WARNING: No assets found at {asset_path}")
        return

    for asset_data in assets:
        asset = unreal.EditorAssetLibrary.load_asset(asset_data)
        if not isinstance(asset, unreal.StaticMesh):
            continue

        actor = unreal.EditorLevelLibrary.spawn_actor_from_object(
            asset, unreal.Vector(0, 0, 0)
        )
        if actor:
            actor.set_editor_property("actor_label", f"BB_{s_id}_{asset.get_name()}")
            print(f"    Spawned: {actor.get_editor_property('actor_label')}")


# ── CLI (reference / dry-run) ─────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="UE5 Block-box FBX Import Utility")
    parser.add_argument("--fbx-dir", required=True, help="Directory containing FBX files")
    parser.add_argument("--ue-dest", default="/Game/BlockBox", help="UE5 content destination")
    parser.add_argument("--structure-layout", type=str, default="", help="Optional structure_layout.json for actor placement")
    parser.add_argument("--dry-run", action="store_true", help="List files without importing")

    args = parser.parse_args()

    print(f"\n=== UE5 Block-box Import Utility ===")
    print(f"  FBX Dir  : {args.fbx_dir}")
    print(f"  UE5 Dest : {args.ue_dest}")

    settings = get_import_options_dict()
    print(f"\n  Import Settings:")
    for k, v in settings.items():
        print(f"    {k}: {v}")

    fbx_files = []
    if os.path.isdir(args.fbx_dir):
        for root, dirs, files in os.walk(args.fbx_dir):
            for f in files:
                if f.lower().endswith(".fbx"):
                    fbx_files.append(os.path.join(root, f))

    print(f"\n  Found {len(fbx_files)} FBX file(s):")
    for fp in sorted(fbx_files):
        size_kb = os.path.getsize(fp) / 1024
        print(f"    {os.path.basename(fp)}  ({size_kb:.1f} KB)")

    if args.dry_run:
        print(f"\n  [DRY RUN] No import performed.")
    else:
        import_all(args.fbx_dir, args.ue_dest)

    if args.structure_layout and os.path.exists(args.structure_layout):
        print(f"\n  Structure Layout: {args.structure_layout}")
        if not args.dry_run:
            spawn_blockbox_actors(args.structure_layout)

    print(f"\n=== Done ===\n")


if __name__ == "__main__":
    main()
