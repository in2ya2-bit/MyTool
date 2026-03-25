# -*- coding: utf-8 -*-
"""
BuildingGenerator 시뮬레이션
─ 구현서의 ETileType, AutoTileLogic, HISM 생성 플로우를 Python으로 재현
─ 3층 건물: 1F 방3 / 2F 방3 / 3F 방2, 각 방 창문1, 층간 계단
"""

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, Rectangle
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import os

OUT_DIR = r"e:\EEE\Shot1\S1\files\output\building_sim"
os.makedirs(OUT_DIR, exist_ok=True)

# ═══════════════════════════════════════════════════════
# ETileType (구현서 03장)
# ═══════════════════════════════════════════════════════
EMPTY      = 0
FLOOR      = 1
WALL       = 2
WALL_DOOR  = 3
WALL_WINDOW= 4
STAIRS     = 5
ROOM_A     = 6
ROOM_B     = 7
ROOM_C     = 8
CORRIDOR   = 9

TILE_LABEL = {
    EMPTY:'Empty', FLOOR:'Floor', WALL:'Wall', WALL_DOOR:'Door',
    WALL_WINDOW:'Window', STAIRS:'Stairs', ROOM_A:'Room A',
    ROOM_B:'Room B', ROOM_C:'Room C', CORRIDOR:'Corridor',
}

TILE_COLOR = {
    EMPTY:'#F5F5F5', FLOOR:'#E0E0E0', WALL:'#4A4A4A',
    WALL_DOOR:'#FFD700', WALL_WINDOW:'#87CEEB', STAIRS:'#FF8C00',
    ROOM_A:'#66BB6A', ROOM_B:'#AB47BC', ROOM_C:'#FF7043',
    CORRIDOR:'#BDBDBD',
}

TILE_LABEL_SHORT = {
    EMPTY:'', FLOOR:'F', WALL:'', WALL_DOOR:'D', WALL_WINDOW:'W',
    STAIRS:'▲', ROOM_A:'A', ROOM_B:'B', ROOM_C:'C', CORRIDOR:'·',
}

STAIR_UP   = 10   # 올라가는 계단
STAIR_DOWN = 11   # 내려오는 계단 (도착)
TILE_LABEL[STAIR_UP]   = 'Stairs Up'
TILE_LABEL[STAIR_DOWN] = 'Stairs Down'
TILE_COLOR[STAIR_UP]   = '#FF8C00'
TILE_COLOR[STAIR_DOWN] = '#E67300'
TILE_LABEL_SHORT[STAIR_UP]   = '▲'
TILE_LABEL_SHORT[STAIR_DOWN] = '▼'

# ═══════════════════════════════════════════════════════
# EWallVariant + AutoTile 비트마스크 (구현서 04장, 16가지 전체)
# ═══════════════════════════════════════════════════════
ISOLATED   = 'Isolated'
STRAIGHT   = 'Straight'
CORNER     = 'Corner'
T_JUNCTION = 'T_Junc'
CROSS      = 'Cross'
END        = 'End'

AUTOTILE_TABLE = {
    0b0000: (ISOLATED,    0),
    0b0001: (END,         0),    # N
    0b0010: (END,       180),    # S
    0b0011: (STRAIGHT,    0),    # N+S
    0b0100: (END,        90),    # E
    0b0101: (CORNER,      0),    # N+E
    0b0110: (CORNER,     90),    # S+E
    0b0111: (T_JUNCTION,  0),    # N+S+E
    0b1000: (END,       270),    # W
    0b1001: (CORNER,    270),    # N+W
    0b1010: (CORNER,    180),    # S+W
    0b1011: (T_JUNCTION,180),    # N+S+W
    0b1100: (STRAIGHT,   90),    # E+W
    0b1101: (T_JUNCTION,270),    # N+E+W
    0b1110: (T_JUNCTION, 90),    # S+E+W
    0b1111: (CROSS,       0),    # N+S+E+W
}

VARIANT_SYMBOL = {
    ISOLATED: '□', STRAIGHT: '─', CORNER: '┐',
    T_JUNCTION: '┤', CROSS: '┼', END: '╴',
}

def is_wall_family(t):
    """구현서: Wall, Wall_Door, Wall_Window 모두 벽 계열"""
    return t in (WALL, WALL_DOOR, WALL_WINDOW)

def autotile_process_floor(grid):
    """구현서 04장: 상하좌우 이웃 분석 → WallVariant + AutoRotationYaw"""
    H, W = grid.shape
    variants = np.full((H, W), '', dtype=object)
    rotations = np.zeros((H, W), dtype=float)

    for y in range(H):
        for x in range(W):
            if not is_wall_family(grid[y, x]):
                continue
            mask = 0
            if y > 0   and is_wall_family(grid[y-1, x]): mask |= 0b0001  # N
            if y < H-1 and is_wall_family(grid[y+1, x]): mask |= 0b0010  # S
            if x < W-1 and is_wall_family(grid[y, x+1]): mask |= 0b0100  # E
            if x > 0   and is_wall_family(grid[y, x-1]): mask |= 0b1000  # W
            variant, yaw = AUTOTILE_TABLE[mask]
            variants[y, x] = variant
            rotations[y, x] = yaw
    return variants, rotations

# ═══════════════════════════════════════════════════════
# 건물 레이아웃 정의 — 타일맵 에디터에서 페인팅하는 과정
# ═══════════════════════════════════════════════════════
GW, GH = 14, 10  # 14칸 x 10칸

def new_grid():
    return np.full((GH, GW), EMPTY, dtype=int)

def fill_rect(g, y1, x1, y2, x2, tile):
    g[y1:y2+1, x1:x2+1] = tile

def walls_around(g, y1, x1, y2, x2):
    """사각형 테두리를 Wall로"""
    g[y1, x1:x2+1] = WALL
    g[y2, x1:x2+1] = WALL
    g[y1:y2+1, x1] = WALL
    g[y1:y2+1, x2] = WALL

def make_1f():
    g = new_grid()
    walls_around(g, 0, 0, 9, 13)

    # ── Room A (top-left) ──
    walls_around(g, 0, 0, 4, 4)
    fill_rect(g, 1, 1, 3, 3, ROOM_A)
    g[2, 4] = WALL_DOOR
    g[2, 0] = WALL_WINDOW

    # ── Room B (top-right) ──
    walls_around(g, 0, 9, 4, 13)
    fill_rect(g, 1, 10, 3, 12, ROOM_B)
    g[2, 9] = WALL_DOOR
    g[2, 13] = WALL_WINDOW

    # ── Room C (bottom-left) ──
    walls_around(g, 6, 0, 9, 7)
    fill_rect(g, 7, 1, 8, 6, ROOM_C)
    g[6, 3] = WALL_DOOR
    g[9, 4] = WALL_WINDOW

    # ── 계단 (bottom-right, 1F→2F) ──
    walls_around(g, 6, 9, 9, 13)
    fill_rect(g, 7, 10, 8, 12, STAIR_UP)
    g[6, 10] = WALL_DOOR

    # ── 복도 ──
    fill_rect(g, 1, 5, 4, 8, CORRIDOR)
    fill_rect(g, 5, 1, 5, 12, CORRIDOR)

    # 복도-하부 구분 벽
    g[6, 1:13] = WALL
    g[6, 3] = WALL_DOOR
    g[6, 10] = WALL_DOOR

    # ── 기둥 (Cross/End 변형 유발) ──
    # 복도 내 구조 기둥: 4방향 벽에 둘러싸여 Cross(┼) 생성
    g[5, 7] = WALL     # 가로 복도 내 기둥 → End (위아래 비벽)
    # 복도 교차점 보강벽: T자/십자 유발
    g[4, 7] = WALL     # 기둥 위에 벽 연결 → Straight
    # 실제 Cross: 상부 복도와 가로 복도 교차점의 내벽
    g[4, 5] = WALL     # Room A 하단벽과 연결된 T자 → 이미 존재

    return g

def make_2f():
    g = new_grid()
    walls_around(g, 0, 0, 9, 13)

    # ── Room A (top-left) ──
    walls_around(g, 0, 0, 4, 4)
    fill_rect(g, 1, 1, 3, 3, ROOM_A)
    g[2, 4] = WALL_DOOR
    g[0, 2] = WALL_WINDOW

    # ── Room B (top-right) ──
    walls_around(g, 0, 9, 4, 13)
    fill_rect(g, 1, 10, 3, 12, ROOM_B)
    g[2, 9] = WALL_DOOR
    g[0, 11] = WALL_WINDOW

    # ── Room C (bottom-left) ──
    walls_around(g, 6, 0, 9, 7)
    fill_rect(g, 7, 1, 8, 6, ROOM_C)
    g[6, 4] = WALL_DOOR
    g[8, 0] = WALL_WINDOW

    # ── 계단 (2F: 1F에서 올라옴 + 3F로 올라감) ──
    walls_around(g, 6, 9, 9, 13)
    fill_rect(g, 7, 10, 7, 12, STAIR_DOWN)   # 1F→2F 도착
    fill_rect(g, 8, 10, 8, 12, STAIR_UP)     # 2F→3F 출발
    g[6, 11] = WALL_DOOR

    # ── 복도 ──
    fill_rect(g, 1, 5, 4, 8, CORRIDOR)
    fill_rect(g, 5, 1, 5, 12, CORRIDOR)
    g[6, 1:13] = WALL
    g[6, 4] = WALL_DOOR
    g[6, 11] = WALL_DOOR

    return g

def make_3f():
    """3F: 방 2개(대형) + 계단 도착 + 복도. footprint 전체 사용."""
    g = new_grid()
    walls_around(g, 0, 0, 9, 13)

    # ── Room A (좌측 대형, 5x4 내부) ──
    walls_around(g, 0, 0, 5, 6)
    fill_rect(g, 1, 1, 4, 5, ROOM_A)
    g[3, 6] = WALL_DOOR
    g[1, 0] = WALL_WINDOW

    # ── Room B (우측 대형, 4x4 내부) ──
    walls_around(g, 0, 8, 5, 13)
    fill_rect(g, 1, 9, 4, 12, ROOM_B)
    g[3, 8] = WALL_DOOR
    g[1, 13] = WALL_WINDOW

    # ── 중앙 세로 복도 (Room A ↔ Room B 사이) ──
    fill_rect(g, 0, 6, 5, 6, WALL)
    fill_rect(g, 0, 8, 5, 8, WALL)
    fill_rect(g, 1, 7, 4, 7, CORRIDOR)
    g[3, 6] = WALL_DOOR
    g[3, 8] = WALL_DOOR

    # ── 상부-하부 구분벽 + 연결 복도 ──
    g[5, :] = WALL
    g[5, 3] = WALL_DOOR
    g[5, 7] = CORRIDOR          # 세로 복도 관통
    g[5, 10] = WALL_DOOR

    # ── 하단 복도 (전체 가로) ──
    fill_rect(g, 6, 1, 6, 12, CORRIDOR)

    # ── 하단 좌측: 발코니/홀 (빈 공간 대신 바닥) ──
    walls_around(g, 7, 0, 9, 8)
    fill_rect(g, 7, 1, 8, 7, CORRIDOR)
    g[7, 4] = WALL                        # 기둥 (Cross 변형 유발)

    # ── 계단 도착 (2F→3F, 같은 XY 위치) ──
    walls_around(g, 7, 9, 9, 13)
    fill_rect(g, 8, 10, 8, 12, STAIR_DOWN)   # 2F→3F 도착
    g[7, 10] = WALL_DOOR

    return g

# ═══════════════════════════════════════════════════════
# 시뮬레이션 실행
# ═══════════════════════════════════════════════════════
floors_data = {
    '1F': make_1f(),
    '2F': make_2f(),
    '3F': make_3f(),
}

# AutoTile 처리
autotile_results = {}
for name, grid in floors_data.items():
    variants, rotations = autotile_process_floor(grid)
    autotile_results[name] = (variants, rotations)

# ═══════════════════════════════════════════════════════
# HISM 버킷 시뮬레이션 (구현서 05장)
# ═══════════════════════════════════════════════════════
class HismBucket:
    def __init__(self, mesh_name):
        self.mesh_name = mesh_name
        self.instances = []  # list of (floor, x, y, yaw, scale)

hism_buckets = {}

def get_or_create_bucket(mesh_name):
    if mesh_name not in hism_buckets:
        hism_buckets[mesh_name] = HismBucket(mesh_name)
    return hism_buckets[mesh_name]

TILE_SIZE = 400      # cm
FLOOR_HEIGHT = 400   # cm

for floor_idx, (floor_name, grid) in enumerate(floors_data.items()):
    variants, rotations = autotile_results[floor_name]
    H, W = grid.shape
    for y in range(H):
        for x in range(W):
            t = grid[y, x]
            if t == EMPTY:
                continue

            world_x = x * TILE_SIZE
            world_y = y * TILE_SIZE
            world_z = floor_idx * FLOOR_HEIGHT

            if t == WALL:
                v = variants[y, x]
                mesh = f"SM_Wall_{v}"
                b = get_or_create_bucket(mesh)
                b.instances.append((floor_idx, x, y, rotations[y, x], (1,1,1)))
            elif t == WALL_DOOR:
                b = get_or_create_bucket("SM_Wall_Door")
                b.instances.append((floor_idx, x, y, rotations[y, x], (1,1,1)))
            elif t == WALL_WINDOW:
                b = get_or_create_bucket("SM_Wall_Window")
                b.instances.append((floor_idx, x, y, rotations[y, x], (1,1,1)))
            elif t in (ROOM_A, ROOM_B, ROOM_C, FLOOR, CORRIDOR):
                mesh = f"SM_Floor_{TILE_LABEL[t].replace(' ','_')}"
                b = get_or_create_bucket(mesh)
                b.instances.append((floor_idx, x, y, 0, (1,1,1)))
            elif t in (STAIRS, STAIR_UP, STAIR_DOWN):
                mesh = "SM_Stairs_Up" if t in (STAIRS, STAIR_UP) else "SM_Stairs_Down"
                b = get_or_create_bucket(mesh)
                b.instances.append((floor_idx, x, y, 0, (1,1,1)))

# ═══════════════════════════════════════════════════════
# HISM 통계 출력
# ═══════════════════════════════════════════════════════
print("=" * 60)
print("  HISM 버킷 통계 (구현서 05장 시뮬레이션)")
print("=" * 60)
total_instances = 0
for name, bucket in sorted(hism_buckets.items()):
    print(f"  {name:30s}  instances: {len(bucket.instances)}")
    total_instances += len(bucket.instances)
print(f"  {'─'*30}  {'─'*15}")
print(f"  {'TOTAL':30s}  instances: {total_instances}")
print(f"  DrawCall 예상: {len(hism_buckets)} (메쉬 종류 수)")
print(f"  Actor 방식 대비: {total_instances}개 Actor → {len(hism_buckets)}개 HISM DrawCall")
print()

# ═══════════════════════════════════════════════════════
# 2D 층별 렌더링
# ═══════════════════════════════════════════════════════
def draw_floor_2d(ax, grid, variants, rotations, floor_name):
    H, W = grid.shape
    ax.set_xlim(-0.5, W - 0.5)
    ax.set_ylim(H - 0.5, -0.5)
    ax.set_aspect('equal')
    ax.set_title(floor_name, fontsize=16, fontweight='bold', pad=10)

    for y in range(H):
        for x in range(W):
            t = grid[y, x]
            color = TILE_COLOR.get(t, '#FFFFFF')
            rect = Rectangle((x - 0.5, y - 0.5), 1, 1,
                              facecolor=color, edgecolor='#888888', linewidth=0.5)
            ax.add_patch(rect)

            label = TILE_LABEL_SHORT.get(t, '')
            if is_wall_family(t) and t == WALL and variants[y, x]:
                label = VARIANT_SYMBOL.get(variants[y, x], '')
            tc = '#FFFFFF' if t in (WALL,) else '#333333'
            if label:
                ax.text(x, y, label, ha='center', va='center',
                        fontsize=7, color=tc, fontweight='bold')

    ax.set_xticks(range(W))
    ax.set_yticks(range(H))
    ax.tick_params(labelsize=6)
    ax.grid(True, linewidth=0.3, alpha=0.5)

fig, axes = plt.subplots(1, 3, figsize=(21, 7))
for idx, (name, grid) in enumerate(floors_data.items()):
    v, r = autotile_results[name]
    draw_floor_2d(axes[idx], grid, v, r, name)

legend_items = [mpatches.Patch(facecolor=TILE_COLOR[k], edgecolor='gray',
                label=TILE_LABEL[k]) for k in
                [WALL, WALL_DOOR, WALL_WINDOW, ROOM_A, ROOM_B, ROOM_C, CORRIDOR, STAIRS]]
fig.legend(handles=legend_items, loc='lower center', ncol=8, fontsize=9,
           frameon=True, edgecolor='gray')
fig.suptitle('BuildingGenerator Simulation — 2D Tilemap (AutoTile Applied)',
             fontsize=14, fontweight='bold', y=0.98)
plt.tight_layout(rect=[0, 0.06, 1, 0.95])
path_2d = os.path.join(OUT_DIR, 'building_sim_2d.png')
fig.savefig(path_2d, dpi=150, bbox_inches='tight')
plt.close(fig)
print(f"✅ 2D 타일맵 저장: {path_2d}")

# ═══════════════════════════════════════════════════════
# AutoTile 결과 상세 렌더링
# ═══════════════════════════════════════════════════════
def draw_autotile_detail(ax, grid, variants, rotations, floor_name):
    H, W = grid.shape
    ax.set_xlim(-0.5, W - 0.5)
    ax.set_ylim(H - 0.5, -0.5)
    ax.set_aspect('equal')
    ax.set_title(f'{floor_name} — AutoTile Variants', fontsize=13, fontweight='bold')

    variant_colors = {
        ISOLATED: '#FF6B6B', STRAIGHT: '#4ECDC4', CORNER: '#45B7D1',
        T_JUNCTION: '#96CEB4', CROSS: '#FFEAA7', END: '#DDA0DD',
    }

    for y in range(H):
        for x in range(W):
            t = grid[y, x]
            if is_wall_family(t):
                v = variants[y, x]
                if t == WALL:
                    color = variant_colors.get(v, '#999')
                elif t == WALL_DOOR:
                    color = TILE_COLOR[WALL_DOOR]
                else:
                    color = TILE_COLOR[WALL_WINDOW]
                rect = Rectangle((x-0.5, y-0.5), 1, 1,
                                  facecolor=color, edgecolor='#555', linewidth=0.8)
                ax.add_patch(rect)
                sym = VARIANT_SYMBOL.get(v, '?')
                rot_label = f"{int(rotations[y,x])}°" if rotations[y,x] != 0 else ''
                ax.text(x, y - 0.15, sym, ha='center', va='center',
                        fontsize=9, fontweight='bold', color='#222')
                if rot_label:
                    ax.text(x, y + 0.25, rot_label, ha='center', va='center',
                            fontsize=5, color='#666')
            else:
                color = '#F0F0F0' if t != EMPTY else '#FAFAFA'
                rect = Rectangle((x-0.5, y-0.5), 1, 1,
                                  facecolor=color, edgecolor='#DDD', linewidth=0.3)
                ax.add_patch(rect)

    ax.set_xticks(range(W))
    ax.set_yticks(range(H))
    ax.tick_params(labelsize=5)
    ax.grid(False)

fig2, axes2 = plt.subplots(1, 3, figsize=(21, 7))
for idx, (name, grid) in enumerate(floors_data.items()):
    v, r = autotile_results[name]
    draw_autotile_detail(axes2[idx], grid, v, r, name)

variant_legend = [mpatches.Patch(facecolor=c, edgecolor='gray', label=f'{k} {VARIANT_SYMBOL.get(k, "")}')
                  for k, c in [
                      (ISOLATED, '#FF6B6B'), (STRAIGHT, '#4ECDC4'), (CORNER, '#45B7D1'),
                      (T_JUNCTION, '#96CEB4'), (CROSS, '#FFEAA7'), (END, '#DDA0DD'),
                  ]]
variant_legend += [
    mpatches.Patch(facecolor=TILE_COLOR[WALL_DOOR], edgecolor='gray', label='Door D'),
    mpatches.Patch(facecolor=TILE_COLOR[WALL_WINDOW], edgecolor='gray', label='Window W'),
]
fig2.legend(handles=variant_legend, loc='lower center', ncol=8, fontsize=9,
            frameon=True, edgecolor='gray')
fig2.suptitle('AutoTile Wall Variant Analysis (Bitmask → Variant + Rotation)',
              fontsize=14, fontweight='bold', y=0.98)
plt.tight_layout(rect=[0, 0.06, 1, 0.95])
path_at = os.path.join(OUT_DIR, 'building_sim_autotile.png')
fig2.savefig(path_at, dpi=150, bbox_inches='tight')
plt.close(fig2)
print(f"✅ AutoTile 상세 저장: {path_at}")

# ═══════════════════════════════════════════════════════
# 3D 아이소메트릭 뷰
# ═══════════════════════════════════════════════════════
def draw_3d_building():
    fig = plt.figure(figsize=(16, 12))
    ax = fig.add_subplot(111, projection='3d')

    floor_gap = 1.5

    for floor_idx, (floor_name, grid) in enumerate(floors_data.items()):
        H, W = grid.shape
        z_base = floor_idx * floor_gap

        for y in range(H):
            for x in range(W):
                t = grid[y, x]
                if t == EMPTY:
                    continue

                color = TILE_COLOR.get(t, '#CCC')
                alpha = 0.9

                if is_wall_family(t):
                    draw_box(ax, x, y, z_base, 1, 1, 1.0, color, alpha=0.85)
                elif t in (STAIRS, STAIR_UP, STAIR_DOWN):
                    h = 0.8 if t == STAIR_UP else 0.5
                    draw_box(ax, x, y, z_base, 1, 1, h, color, alpha=0.9)
                else:
                    # 바닥: 낮은 블록
                    draw_box(ax, x, y, z_base, 1, 1, 0.08, color, alpha=0.7)

    ax.set_xlabel('X', fontsize=10)
    ax.set_ylabel('Y', fontsize=10)
    ax.set_zlabel('Floor', fontsize=10)
    ax.set_title('BuildingGenerator — 3D Isometric View\n(3 Floors, HISM Rendering Simulation)',
                 fontsize=14, fontweight='bold')

    ax.view_init(elev=25, azim=-55)
    ax.set_box_aspect([GW, GH, len(floors_data) * floor_gap])

    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_zticks([])

    legend_items = [mpatches.Patch(facecolor=TILE_COLOR[k], edgecolor='gray',
                    label=TILE_LABEL[k]) for k in
                    [WALL, WALL_DOOR, WALL_WINDOW, ROOM_A, ROOM_B, ROOM_C, CORRIDOR, STAIRS]]
    floor_labels = [mpatches.Patch(facecolor='white', edgecolor='black',
                    label=f'{name} (z={i*1.5:.1f})')
                    for i, name in enumerate(['1F','2F','3F'])]
    ax.legend(handles=legend_items + floor_labels, loc='upper left',
              fontsize=8, framealpha=0.9, ncol=2)

    return fig

def draw_box(ax, x, y, z, dx, dy, dz, color, alpha=0.8):
    """직육면체 그리기"""
    xx = [x, x+dx, x+dx, x, x, x+dx, x+dx, x]
    yy = [y, y, y+dy, y+dy, y, y, y+dy, y+dy]
    zz = [z, z, z, z, z+dz, z+dz, z+dz, z+dz]

    faces = [
        [0,1,5,4], [2,3,7,6],  # front, back
        [0,3,7,4], [1,2,6,5],  # left, right
        [4,5,6,7], [0,1,2,3],  # top, bottom
    ]

    verts = [[xx[i], yy[i], zz[i]] for i in range(8)]
    for face in faces:
        poly = Poly3DCollection([[verts[i] for i in face]])
        poly.set_facecolor(color)
        poly.set_edgecolor('#666666')
        poly.set_alpha(alpha)
        poly.set_linewidth(0.3)
        ax.add_collection3d(poly)

fig3 = draw_3d_building()
path_3d = os.path.join(OUT_DIR, 'building_sim_3d.png')
fig3.savefig(path_3d, dpi=150, bbox_inches='tight')
plt.close(fig3)
print(f"✅ 3D 뷰 저장: {path_3d}")

# ═══════════════════════════════════════════════════════
# 종합 리포트 출력
# ═══════════════════════════════════════════════════════
print()
print("=" * 60)
print("  BuildingGenerator 시뮬레이션 완료")
print("=" * 60)

for name, grid in floors_data.items():
    H, W = grid.shape
    variants, _ = autotile_results[name]
    wall_count = np.sum(grid == WALL)
    door_count = np.sum(grid == WALL_DOOR)
    window_count = np.sum(grid == WALL_WINDOW)
    floor_count = sum(np.sum(grid == t) for t in [ROOM_A, ROOM_B, ROOM_C, FLOOR, CORRIDOR])
    stair_count = np.sum(grid == STAIRS) + np.sum(grid == STAIR_UP) + np.sum(grid == STAIR_DOWN)

    v_counts = {}
    for y in range(H):
        for x in range(W):
            if variants[y, x]:
                v = variants[y, x]
                v_counts[v] = v_counts.get(v, 0) + 1

    print(f"\n  {name} ({W}x{H} = {W*H} tiles)")
    print(f"    Wall: {wall_count}  Door: {door_count}  Window: {window_count}  Floor: {floor_count}  Stairs: {stair_count}")
    print(f"    AutoTile: {v_counts}")

print(f"\n  HISM 버킷: {len(hism_buckets)}개 → DrawCall: {len(hism_buckets)}")
print(f"  총 인스턴스: {total_instances}개")
print(f"  Actor 방식 대비 DrawCall 절감: {total_instances} → {len(hism_buckets)} ({(1-len(hism_buckets)/total_instances)*100:.1f}% 감소)")
print()
print(f"  출력 파일:")
print(f"    {path_2d}")
print(f"    {path_at}")
print(f"    {path_3d}")
