"""
BuildingGenerator — 블록 레벨 플레이스홀더 메시 생성 (Blender Script)

실행:
  blender --background --python files/create_building_meshes.py

출력:
  files/output/building_meshes/*.fbx  (15개 메시)

UE5 Import:
  1. Content Browser > Import > 폴더 내 모든 .fbx 선택
  2. Import All (기본 설정 OK)
  3. DA_BuildingMeshSet (UBuildingDataAsset) 생성
  4. 각 슬롯에 메시 할당, PivotOffset = (200, 200, 0)

메시 목록:
  Wall: Isolated, End, Straight, Corner, T_Junction, Cross
  Door, Window
  Floor: Generic, Room_A, Room_B, Room_C, Corridor
  Stairs: Up, Down

좌표계: 메시 원점 = 타일 중앙(XY) / 바닥(Z=0)
타일 크기 4m, 벽 높이 3m, 벽 두께 20cm
"""

import bpy
import bmesh
import os

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "output", "building_meshes")
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ═══════════════════════════════════════════════════════
# Dimensions (meters)
# ═══════════════════════════════════════════════════════
TILE = 4.0
HALF = TILE / 2          # 2.0
WT   = 0.40              # wall thickness (wider for better corner coverage)
WHT  = WT / 2            # 0.20
WH   = 3.0               # wall height
FT   = 0.05              # floor slab thickness

DOOR_W  = 1.2
DOOR_H  = 2.4
DOOR_HW = DOOR_W / 2

WIN_W   = 1.0
WIN_H   = 1.0
WIN_BOT = 1.2
WIN_HW  = WIN_W / 2

# ═══════════════════════════════════════════════════════
# Helpers
# ═══════════════════════════════════════════════════════

def clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)
    for m in list(bpy.data.materials):
        bpy.data.materials.remove(m)


def add_box(bm, x1, y1, z1, x2, y2, z2):
    """Add an axis-aligned box to a bmesh."""
    v = [
        bm.verts.new((x1, y1, z1)),
        bm.verts.new((x2, y1, z1)),
        bm.verts.new((x2, y2, z1)),
        bm.verts.new((x1, y2, z1)),
        bm.verts.new((x1, y1, z2)),
        bm.verts.new((x2, y1, z2)),
        bm.verts.new((x2, y2, z2)),
        bm.verts.new((x1, y2, z2)),
    ]
    for indices in [
        (3, 2, 1, 0),  # bottom
        (4, 5, 6, 7),  # top
        (0, 1, 5, 4),  # front  (-Y)
        (2, 3, 7, 6),  # back   (+Y)
        (0, 4, 7, 3),  # left   (-X)
        (1, 2, 6, 5),  # right  (+X)
    ]:
        try:
            bm.faces.new([v[i] for i in indices])
        except ValueError:
            pass


def build_and_export(name, build_fn, color):
    """Clear scene, build mesh, assign material, export FBX."""
    clear_scene()

    bm = bmesh.new()
    build_fn(bm)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.001)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)

    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)

    mat = bpy.data.materials.new(name + "_Mat")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = color
    obj.data.materials.append(mat)

    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj

    filepath = os.path.join(OUTPUT_DIR, f"{name}.fbx")
    bpy.ops.export_scene.fbx(
        filepath=filepath,
        use_selection=True,
        global_scale=1.0,
        apply_scale_options='FBX_SCALE_NONE',
        axis_forward='-Y',
        axis_up='Z',
        mesh_smooth_type='OFF',
        add_leaf_bones=False,
        bake_anim=False,
    )
    print(f"  >> {name}.fbx  ({len(mesh.vertices)} verts)")


# ═══════════════════════════════════════════════════════
# Wall Variant Builders
# ═══════════════════════════════════════════════════════
# 기본 방향: 벽 면이 Y축을 따라 연장 (N-S 방향 = Yaw 0°)
# AutoTile Yaw 회전으로 다른 방향 생성

def wall_isolated(bm):
    """Isolated (0000): 독립 기둥"""
    s = 0.20
    add_box(bm, -s, -s, 0, s, s, WH)


def wall_end(bm):
    """End (0001=N): 한쪽만 연결, -Y 방향으로 반타일"""
    add_box(bm, -WHT, -HALF, 0, WHT, 0, WH)


def wall_straight(bm):
    """Straight (0011=N+S): Y축 전체 관통"""
    add_box(bm, -WHT, -HALF, 0, WHT, HALF, WH)


def wall_corner(bm):
    """Corner (0101=N+E): L자 — N(-Y)과 E(+X) 방향"""
    add_box(bm, -WHT, -HALF, 0,  WHT, WHT, WH)   # N arm
    add_box(bm, -WHT, -WHT,  0, HALF, WHT, WH)   # E arm


def wall_t_junction(bm):
    """T_Junction (0111=N+S+E): T자 — N-S 직선 + E arm"""
    add_box(bm, -WHT, -HALF, 0, WHT,  HALF, WH)  # N-S
    add_box(bm,  WHT, -WHT,  0, HALF,  WHT, WH)  # E arm


def wall_cross(bm):
    """Cross (1111=NSEW): 십자 — N-S + E-W 관통"""
    add_box(bm, -WHT,  -HALF, 0,  WHT, HALF, WH)  # N-S
    add_box(bm, -HALF, -WHT,  0, HALF,  WHT, WH)  # E-W


def wall_door(bm):
    """Door: Straight 벽 + 중앙 문 개구부 (1.2m × 2.4m)"""
    add_box(bm, -WHT, -HALF,    0, WHT, -DOOR_HW, WH)      # left pillar
    add_box(bm, -WHT,  DOOR_HW, 0, WHT,  HALF,    WH)      # right pillar
    add_box(bm, -WHT, -DOOR_HW, DOOR_H, WHT, DOOR_HW, WH)  # lintel


def wall_window(bm):
    """Window: Straight 벽 + 중앙 창 개구부 (1.0m × 1.0m, sill 1.2m)"""
    win_top = WIN_BOT + WIN_H
    add_box(bm, -WHT, -HALF, 0,       WHT, HALF, WIN_BOT)   # below sill
    add_box(bm, -WHT, -HALF, win_top, WHT, HALF, WH)        # above window
    add_box(bm, -WHT, -HALF, WIN_BOT, WHT, -WIN_HW, win_top)  # left jam
    add_box(bm, -WHT,  WIN_HW, WIN_BOT, WHT, HALF, win_top)   # right jam


# ═══════════════════════════════════════════════════════
# Floor Builders
# ═══════════════════════════════════════════════════════

def floor_tile(bm):
    """바닥 슬래브: 4m × 4m × 5cm"""
    add_box(bm, -HALF, -HALF, 0, HALF, HALF, FT)


# ═══════════════════════════════════════════════════════
# Stair Builders
# ═══════════════════════════════════════════════════════

STAIR_W = 0.60   # half-width of stairs (1.2m total)

def stairs_up(bm):
    """올라가는 계단: +Y(남) → -Y(북), 8단, 폭 1.2m"""
    steps = 8
    sd = TILE / steps
    sh = WH / steps
    for i in range(steps):
        y_bot = HALF - (i + 1) * sd
        y_top = HALF - i * sd
        add_box(bm, -STAIR_W, y_bot, 0, STAIR_W, y_top, (i + 1) * sh)


def stairs_down(bm):
    """내려오는 계단: -Y(북, 높은쪽) → +Y(남, 낮은쪽), 8단, 폭 1.2m"""
    steps = 8
    sd = TILE / steps
    sh = WH / steps
    for i in range(steps):
        y_bot = -HALF + i * sd
        y_top = -HALF + (i + 1) * sd
        add_box(bm, -STAIR_W, y_bot, 0, STAIR_W, y_top, WH - i * sh)


# ═══════════════════════════════════════════════════════
# Main — Generate All Meshes
# ═══════════════════════════════════════════════════════

MESHES = [
    # Walls
    ("SM_Wall_Isolated",   wall_isolated,   (0.60, 0.60, 0.60, 1)),
    ("SM_Wall_End",        wall_end,        (0.50, 0.50, 0.50, 1)),
    ("SM_Wall_Straight",   wall_straight,   (0.40, 0.40, 0.40, 1)),
    ("SM_Wall_Corner",     wall_corner,     (0.45, 0.45, 0.50, 1)),
    ("SM_Wall_T_Junction", wall_t_junction, (0.50, 0.45, 0.45, 1)),
    ("SM_Wall_Cross",      wall_cross,      (0.55, 0.50, 0.45, 1)),
    ("SM_Wall_Door",       wall_door,       (0.80, 0.65, 0.00, 1)),
    ("SM_Wall_Window",     wall_window,     (0.40, 0.60, 0.80, 1)),
    # Floors (same geometry, different materials)
    ("SM_Floor_Generic",   floor_tile,      (0.70, 0.70, 0.70, 1)),
    ("SM_Floor_Room_A",    floor_tile,      (0.30, 0.65, 0.30, 1)),
    ("SM_Floor_Room_B",    floor_tile,      (0.55, 0.22, 0.65, 1)),
    ("SM_Floor_Room_C",    floor_tile,      (0.85, 0.42, 0.22, 1)),
    ("SM_Floor_Corridor",  floor_tile,      (0.55, 0.55, 0.55, 1)),
    # Stairs
    ("SM_Stairs_Up",       stairs_up,       (0.80, 0.50, 0.00, 1)),
    ("SM_Stairs_Down",     stairs_down,     (0.70, 0.40, 0.00, 1)),
]

print("=" * 50)
print("  BuildingGenerator Placeholder Mesh Export")
print("=" * 50)

for name, builder, color in MESHES:
    build_and_export(name, builder, color)

print(f"\n{'=' * 50}")
print(f"  {len(MESHES)} meshes -> {OUTPUT_DIR}")
print(f"{'=' * 50}")
print()
print("UE5 Setup:")
print("  1. Content Browser > Import > select all .fbx")
print("  2. Create Data Asset: UBuildingDataAsset")
print("  3. WallMeshes map:")
print("     Isolated   -> SM_Wall_Isolated")
print("     End        -> SM_Wall_End")
print("     Straight   -> SM_Wall_Straight")
print("     Corner     -> SM_Wall_Corner")
print("     T_Junction -> SM_Wall_T_Junction")
print("     Cross      -> SM_Wall_Cross")
print("  4. DoorMesh    -> SM_Wall_Door")
print("  5. WindowMesh  -> SM_Wall_Window")
print("  6. FloorMeshes: Floor->Generic, Room_A/B/C, Corridor")
print("  7. StairsUpMesh -> SM_Stairs_Up")
print("  8. StairsDownMesh -> SM_Stairs_Down")
print("  9. ALL slots: PivotOffset = (200, 200, 0)")
