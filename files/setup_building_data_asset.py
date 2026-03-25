"""
BuildingGenerator — UE5 에디터 Python 스크립트
FBX 임포트 + DA_BuildingAsset 메시 등록

실행: UE5 에디터 > Output Log > Python 콘솔에서:
  exec(open(r"e:\EEE\Shot1\S1\files\setup_building_data_asset.py").read())
"""

import unreal
import os

# ═══════════════════════════════════════════════════════
# Config
# ═══════════════════════════════════════════════════════
FBX_DIR = r"e:\EEE\Shot1\S1\files\output\building_meshes"
IMPORT_PATH = "/Game/BuildingGenerator/Meshes"
PIVOT = unreal.Vector(200.0, 200.0, 0.0)
SCALE = unreal.Vector(1.0, 1.0, 1.0)

# DataAsset 자동 검색
DA_PATH = None
DA_SEARCH_PATHS = [
    "/Game/LevelTool/Data/DA_BuildingAsset",
    "/Game/LevelTool/DA_BuildingAsset",
    "/Game/BuildingGenerator/DA_BuildingAsset",
    "/Game/Data/DA_BuildingAsset",
]

# ═══════════════════════════════════════════════════════
# Step 1: Import FBX files
# ═══════════════════════════════════════════════════════
print("=" * 50)
print("  Step 1: Importing FBX meshes")
print("=" * 50)

fbx_files = [f for f in os.listdir(FBX_DIR) if f.endswith(".fbx")]
tasks = []

for fbx in fbx_files:
    full_path = os.path.join(FBX_DIR, fbx)
    asset_name = os.path.splitext(fbx)[0]

    # Skip if already imported
    if unreal.EditorAssetLibrary.does_asset_exist(f"{IMPORT_PATH}/{asset_name}"):
        print(f"  [skip] {asset_name} (already exists)")
        continue

    task = unreal.AssetImportTask()
    task.filename = full_path
    task.destination_path = IMPORT_PATH
    task.destination_name = asset_name
    task.automated = True
    task.replace_existing = True
    task.save = False

    # FBX import options
    opts = unreal.FbxImportUI()
    opts.import_mesh = True
    opts.import_as_skeletal = False
    opts.import_animations = False
    opts.import_materials = True
    opts.static_mesh_import_data.combine_meshes = True
    opts.static_mesh_import_data.auto_generate_collision = False
    task.options = opts

    tasks.append(task)
    print(f"  [import] {asset_name}")

if tasks:
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    print(f"  Imported {len(tasks)} meshes")
else:
    print("  All meshes already imported")

# ═══════════════════════════════════════════════════════
# Step 2: Find & Load DataAsset
# ═══════════════════════════════════════════════════════
print()
print("=" * 50)
print("  Step 2: Finding DA_BuildingAsset")
print("=" * 50)

da = None

# Method 1: try known paths
for path in DA_SEARCH_PATHS:
    try:
        obj = unreal.load_asset(path)
        if obj is not None:
            da = obj
            DA_PATH = path
            print(f"  Found at: {path}")
            break
    except:
        pass

# Method 2: search AssetRegistry by name
if da is None:
    print("  Searching AssetRegistry...")
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    all_assets = registry.get_all_assets()
    for asset_data in all_assets:
        name = str(asset_data.asset_name)
        if "DA_Building" in name:
            pkg = str(asset_data.package_name)
            print(f"  Found candidate: {pkg} ({name})")
            try:
                da = unreal.load_asset(pkg)
                DA_PATH = pkg
                break
            except:
                pass

if da is None:
    print("  DataAsset not found. Creating new one...")
    try:
        factory = unreal.DataAssetFactory()
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        da = asset_tools.create_asset("DA_BuildingAsset", "/Game/BuildingGenerator",
                                       unreal.BuildingDataAsset, factory)
        DA_PATH = "/Game/BuildingGenerator/DA_BuildingAsset"
        print(f"  Created: {DA_PATH}")
    except Exception as e:
        print(f"  Create failed: {e}")
        print("  >> Manually create: Content Browser > Add > Data Asset > BuildingDataAsset")

if da is not None:
    print(f"  DataAsset ready: {DA_PATH} (type: {type(da).__name__})")

# ═══════════════════════════════════════════════════════
# Helper: create FMeshSlot
# ═══════════════════════════════════════════════════════
def make_slot(mesh_name, pivot=PIVOT, scale=SCALE):
    mesh_path = f"{IMPORT_PATH}/{mesh_name}.{mesh_name}"
    mesh = unreal.load_asset(f"{IMPORT_PATH}/{mesh_name}")
    if mesh is None:
        print(f"    WARNING: mesh not found: {mesh_name}")

    slot = unreal.MeshSlot()
    slot.set_editor_property("Mesh", unreal.SoftObjectPath(f"{IMPORT_PATH}/{mesh_name}.{mesh_name}"))
    slot.set_editor_property("ScaleCorrection", scale)
    slot.set_editor_property("PivotOffset", pivot)
    return slot

# ═══════════════════════════════════════════════════════
# Step 3: Assign Wall meshes (TMap<EWallVariant, FMeshSlot>)
# ═══════════════════════════════════════════════════════
if da is not None:
    print()
    print("  Assigning wall meshes...")

    wall_map = {
        unreal.EWallVariant.ISOLATED:   make_slot("SM_Wall_Isolated"),
        unreal.EWallVariant.END:        make_slot("SM_Wall_End"),
        unreal.EWallVariant.STRAIGHT:   make_slot("SM_Wall_Straight"),
        unreal.EWallVariant.CORNER:     make_slot("SM_Wall_Corner"),
        unreal.EWallVariant.T_JUNCTION: make_slot("SM_Wall_T_Junction"),
        unreal.EWallVariant.CROSS:      make_slot("SM_Wall_Cross"),
    }

    try:
        da.set_editor_property("WallMeshes", wall_map)
        print("    WallMeshes: OK")
    except Exception as e:
        print(f"    WallMeshes via set_editor_property failed: {e}")
        print("    Trying alternative method...")
        try:
            wm = da.get_editor_property("WallMeshes")
            for k, v in wall_map.items():
                wm[k] = v
            print("    WallMeshes: OK (alternative)")
        except Exception as e2:
            print(f"    WallMeshes alternative also failed: {e2}")
            print("    >> Please assign WallMeshes manually in the editor")

    # ── Door / Window ──
    print("  Assigning door & window meshes...")
    try:
        da.set_editor_property("DoorMesh", make_slot("SM_Wall_Door"))
        print("    DoorMesh: OK")
    except Exception as e:
        print(f"    DoorMesh failed: {e}")

    try:
        da.set_editor_property("WindowMesh", make_slot("SM_Wall_Window"))
        print("    WindowMesh: OK")
    except Exception as e:
        print(f"    WindowMesh failed: {e}")

    # ── Floor meshes (TMap<ETileType, FMeshSlot>) ──
    print("  Assigning floor meshes...")

    floor_map = {
        unreal.ETileType.FLOOR:    make_slot("SM_Floor_Generic"),
        unreal.ETileType.ROOM_A:   make_slot("SM_Floor_Room_A"),
        unreal.ETileType.ROOM_B:   make_slot("SM_Floor_Room_B"),
        unreal.ETileType.ROOM_C:   make_slot("SM_Floor_Room_C"),
        unreal.ETileType.CORRIDOR: make_slot("SM_Floor_Corridor"),
    }

    try:
        da.set_editor_property("FloorMeshes", floor_map)
        print("    FloorMeshes: OK")
    except Exception as e:
        print(f"    FloorMeshes via set_editor_property failed: {e}")
        try:
            fm = da.get_editor_property("FloorMeshes")
            for k, v in floor_map.items():
                fm[k] = v
            print("    FloorMeshes: OK (alternative)")
        except Exception as e2:
            print(f"    FloorMeshes alternative also failed: {e2}")
            print("    >> Please assign FloorMeshes manually in the editor")

    # ── Stairs ──
    print("  Assigning stair meshes...")
    try:
        da.set_editor_property("StairsUpMesh", make_slot("SM_Stairs_Up"))
        print("    StairsUpMesh: OK")
    except Exception as e:
        print(f"    StairsUpMesh failed: {e}")

    try:
        da.set_editor_property("StairsDownMesh", make_slot("SM_Stairs_Down"))
        print("    StairsDownMesh: OK")
    except Exception as e:
        print(f"    StairsDownMesh failed: {e}")

    # ── Fallback ──
    print("  Assigning fallback mesh...")
    try:
        da.set_editor_property("FallbackMesh",
            make_slot("SM_Wall_Straight"))
        print("    FallbackMesh: OK")
    except Exception as e:
        print(f"    FallbackMesh failed: {e}")

    # ── Save ──
    print()
    print("  Saving DataAsset...")
    unreal.EditorAssetLibrary.save_asset(DA_PATH)
    print("  DONE!")

print()
print("=" * 50)
print("  Setup complete")
print("=" * 50)
print()
print("  Next: Select BuildingActor > Details > MeshSet > DA_BuildingAsset")
print("  Then: Building Editor > paint tiles > Rebuild HISM")
