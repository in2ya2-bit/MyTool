"""
Visualize floorplan.json samples as 2D plans and 3D block-box previews.
Usage: python visualize_blockbox.py
"""
import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from mpl_toolkits.mplot3d import Axes3D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import numpy as np

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

COLORS = {
    "exterior":  "#5B7FAF",
    "interior":  "#C4A35A",
    "partition":  "#AAAAAA",
    "door":      "#E07040",
    "window":    "#60C0E0",
    "shutter":   "#D09040",
    "floor":     "#D0CCC0",
    "stair":     "#70B870",
    "column":    "#C06060",
    "room_fill": [
        "#E8E4D8", "#D6E8D0", "#D0D8E8", "#E8D8D0",
        "#E0E0C8", "#D0E0E0", "#E8D0E0", "#E0D8C0",
        "#C8D8D0", "#D8C8D8", "#D0D0B8", "#C8C0D0",
    ],
}

BRIDGE_COLORS = {
    "deck":      "#7888A0",
    "sidewalk":  "#A0A8B0",
    "pier":      "#907060",
    "abutment":  "#806858",
    "railing":   "#606060",
}


# ─── 2D Floor Plan ────────────────────────────────────────────────────────────

def draw_floor_2d(ax, floor, title, building_name=""):
    ax.set_aspect("equal")
    ax.set_title(f"{building_name} — {title}", fontsize=11, fontweight="bold", pad=8)
    ax.set_xlabel("X (m)")
    ax.set_ylabel("Y (m)")
    ax.grid(True, alpha=0.2)

    for i, room in enumerate(floor.get("rooms", [])):
        poly = room["polygon"]
        if len(poly) < 3:
            continue
        color = COLORS["room_fill"][i % len(COLORS["room_fill"])]
        xs = [p[0] for p in poly] + [poly[0][0]]
        ys = [p[1] for p in poly] + [poly[0][1]]
        ax.fill(xs, ys, color=color, alpha=0.5, zorder=1)
        cx = sum(p[0] for p in poly) / len(poly)
        cy = sum(p[1] for p in poly) / len(poly)
        label = room.get("label", room["room_id"])
        ax.text(cx, cy, label, ha="center", va="center", fontsize=6,
                fontweight="bold", zorder=5, color="#333333")

    for wall in floor.get("walls", []):
        sx, sy = wall["start"]
        ex, ey = wall["end"]
        t = wall["thickness_m"]
        wtype = wall.get("type", "interior")
        color = COLORS.get(wtype, "#666666")
        lw = max(1.5, t * 12)
        ax.plot([sx, ex], [sy, ey], color=color, linewidth=lw, solid_capstyle="round", zorder=3)

        length = math.sqrt((ex - sx)**2 + (ey - sy)**2)
        dx = (ex - sx) / length if length > 0 else 0
        dy = (ey - sy) / length if length > 0 else 0

        for opening in wall.get("openings", []):
            offset = opening["offset_m"]
            w = opening["width_m"]
            otype = opening["type"]
            o_sx = sx + dx * offset
            o_sy = sy + dy * offset
            o_ex = sx + dx * (offset + w)
            o_ey = sy + dy * (offset + w)
            oc = COLORS.get(otype, "#FF00FF")
            ax.plot([o_sx, o_ex], [o_sy, o_ey], color=oc, linewidth=lw + 1,
                    solid_capstyle="butt", zorder=4)
            if otype == "door":
                r = w * 0.8
                angle_base = math.degrees(math.atan2(dy, dx))
                arc = patches.Arc(
                    (o_sx, o_sy), r * 2, r * 2,
                    angle=angle_base, theta1=0, theta2=90,
                    color=oc, linewidth=0.8, zorder=4
                )
                ax.add_patch(arc)

    for stair in floor.get("stairs", []):
        loc = stair["location"]
        sw, sd = stair["size_m"]
        rect = patches.FancyBboxPatch(
            (loc[0] - sw/2, loc[1] - sd/2), sw, sd,
            boxstyle="round,pad=0.1",
            linewidth=1.2, edgecolor=COLORS["stair"],
            facecolor=COLORS["stair"], alpha=0.35, zorder=2
        )
        ax.add_patch(rect)
        ax.text(loc[0], loc[1], "▲", ha="center", va="center",
                fontsize=8, color=COLORS["stair"], zorder=5)

    for col in floor.get("columns", []):
        cx, cy = col["center"]
        s = col["size_m"][0]
        if col.get("shape") == "circle":
            circle = plt.Circle((cx, cy), s/2, color=COLORS["column"],
                                alpha=0.6, zorder=3)
            ax.add_patch(circle)
        else:
            rect = patches.Rectangle(
                (cx - s/2, cy - s/2), s, s,
                color=COLORS["column"], alpha=0.6, zorder=3
            )
            ax.add_patch(rect)

    legend_items = [
        patches.Patch(color=COLORS["exterior"], label="Exterior Wall"),
        patches.Patch(color=COLORS["interior"], label="Interior Wall"),
        patches.Patch(color=COLORS["partition"], label="Partition"),
        patches.Patch(color=COLORS["door"], label="Door"),
        patches.Patch(color=COLORS["window"], label="Window"),
        patches.Patch(color=COLORS["stair"], label="Stairs"),
        patches.Patch(color=COLORS["column"], label="Column"),
    ]
    ax.legend(handles=legend_items, loc="upper right", fontsize=5,
              framealpha=0.8)


def visualize_building_2d(data, output_path):
    floors = data.get("floors", [])
    if not floors:
        return
    n = len(floors)
    fig, axes = plt.subplots(1, n, figsize=(7 * n, 7))
    if n == 1:
        axes = [axes]

    name = data.get("building_name", "Building")
    for i, floor in enumerate(floors):
        draw_floor_2d(axes[i], floor, floor.get("label", f"Floor {i}"), name)

    plt.tight_layout()
    fig.savefig(output_path, dpi=180, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print(f"  Saved 2D plan: {output_path}")


# ─── 3D Block-box ─────────────────────────────────────────────────────────────

def box_faces(x0, y0, z0, x1, y1, z1):
    v = np.array([
        [x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0],
        [x0,y0,z1],[x1,y0,z1],[x1,y1,z1],[x0,y1,z1],
    ])
    faces = [
        [v[0],v[1],v[5],v[4]], [v[2],v[3],v[7],v[6]],
        [v[0],v[3],v[7],v[4]], [v[1],v[2],v[6],v[5]],
        [v[0],v[1],v[2],v[3]], [v[4],v[5],v[6],v[7]],
    ]
    return faces


def add_wall_3d(ax, wall, elev, h, color, alpha=0.7):
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    t = wall["thickness_m"]
    length = math.sqrt((ex-sx)**2 + (ey-sy)**2)
    if length < 0.01:
        return
    dx = (ex - sx) / length
    dy = (ey - sy) / length
    nx, ny = -dy, dx

    corners = [
        (sx + nx*t/2, sy + ny*t/2),
        (ex + nx*t/2, ey + ny*t/2),
        (ex - nx*t/2, ey - ny*t/2),
        (sx - nx*t/2, sy - ny*t/2),
    ]

    bottom = [(c[0], c[1], elev) for c in corners]
    top = [(c[0], c[1], elev + h) for c in corners]

    faces = [
        bottom[::-1], top,
        [bottom[0], bottom[1], top[1], top[0]],
        [bottom[1], bottom[2], top[2], top[1]],
        [bottom[2], bottom[3], top[3], top[2]],
        [bottom[3], bottom[0], top[0], top[3]],
    ]
    poly = Poly3DCollection(faces, alpha=alpha, facecolor=color,
                            edgecolor="#333333", linewidth=0.3)
    ax.add_collection3d(poly)


def add_floor_plate_3d(ax, rooms, elev, thickness=0.15, color="#C8C4B8", alpha=0.4):
    for room in rooms:
        poly = room.get("polygon", [])
        if len(poly) < 3:
            continue
        bottom = [(p[0], p[1], elev) for p in poly]
        top = [(p[0], p[1], elev + thickness) for p in poly]
        faces = [bottom[::-1], top]
        n = len(poly)
        for i in range(n):
            j = (i + 1) % n
            faces.append([bottom[i], bottom[j], top[j], top[i]])
        pc = Poly3DCollection(faces, alpha=alpha, facecolor=color,
                              edgecolor="#888888", linewidth=0.2)
        ax.add_collection3d(pc)


def add_stair_3d(ax, stair, elev, h, color="#40B040", alpha=0.9):
    loc = stair["location"]
    sw, sd = stair["size_m"]
    x0, y0 = loc[0] - sw/2, loc[1] - sd/2
    faces = box_faces(x0, y0, elev, x0+sw, y0+sd, elev+h)
    pc = Poly3DCollection(faces, alpha=alpha, facecolor=color,
                          edgecolor="#206020", linewidth=1.0, zorder=10)
    ax.add_collection3d(pc)


def add_column_3d(ax, col, elev, h, color=COLORS["column"], alpha=0.6):
    cx, cy = col["center"]
    s = col["size_m"][0]
    faces = box_faces(cx-s/2, cy-s/2, elev, cx+s/2, cy+s/2, elev+h)
    pc = Poly3DCollection(faces, alpha=alpha, facecolor=color,
                          edgecolor="#663333", linewidth=0.3)
    ax.add_collection3d(pc)


def visualize_building_3d(data, output_path):
    floors = data.get("floors", [])
    if not floors:
        return

    fig = plt.figure(figsize=(14, 10))
    ax = fig.add_subplot(111, projection="3d")
    name = data.get("building_name", "Building")
    ax.set_title(f"{name} — 3D Block-box Preview", fontsize=13, fontweight="bold", pad=12)

    all_x, all_y, all_z = [], [], []

    for floor in floors:
        elev = floor["elevation_m"]
        h = floor["height_m"]

        add_floor_plate_3d(ax, floor.get("rooms", []), elev)
        add_floor_plate_3d(ax, floor.get("rooms", []), elev + h - 0.15,
                           color="#D0CCC0", alpha=0.25)

        for wall in floor.get("walls", []):
            wtype = wall.get("type", "interior")
            color = COLORS.get(wtype, "#888888")
            add_wall_3d(ax, wall, elev, h, color, alpha=0.55)
            for p in [wall["start"], wall["end"]]:
                all_x.append(p[0]); all_y.append(p[1])
            all_z.extend([elev, elev + h])

        for stair in floor.get("stairs", []):
            add_stair_3d(ax, stair, elev, h)

        for col in floor.get("columns", []):
            add_column_3d(ax, col, elev, h)

    if all_x:
        margin = 3
        ax.set_xlim(min(all_x) - margin, max(all_x) + margin)
        ax.set_ylim(min(all_y) - margin, max(all_y) + margin)
        ax.set_zlim(min(all_z) - 1, max(all_z) + 2)

    ax.set_xlabel("X (m)")
    ax.set_ylabel("Y (m)")
    ax.set_zlabel("Z (m)")
    ax.view_init(elev=28, azim=-55)

    legend_items = [
        patches.Patch(color=COLORS["exterior"], label="Exterior"),
        patches.Patch(color=COLORS["interior"], label="Interior"),
        patches.Patch(color=COLORS["partition"], label="Partition"),
        patches.Patch(color=COLORS["stair"], label="Stairs"),
        patches.Patch(color=COLORS["column"], label="Column"),
        patches.Patch(color="#C8C4B8", label="Floor plate"),
    ]
    ax.legend(handles=legend_items, loc="upper left", fontsize=7, framealpha=0.8)

    fig.savefig(output_path, dpi=180, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print(f"  Saved 3D view: {output_path}")


# ─── Bridge ───────────────────────────────────────────────────────────────────

def visualize_bridge_3d(data, output_path):
    bridges = data.get("bridges", [])
    if not bridges:
        return

    fig = plt.figure(figsize=(16, 8))
    ax = fig.add_subplot(111, projection="3d")
    ax.set_title("Bridge Block-box Preview", fontsize=13, fontweight="bold", pad=12)

    all_x, all_y, all_z = [], [], []

    for bridge in bridges:
        path = np.array(bridge["path"])
        deck = bridge["deck"]
        w = deck["width_m"]
        thick = deck["thickness_m"]
        sw_w = deck.get("sidewalk_width_m", 0)
        sw_h = deck.get("sidewalk_height_offset_m", 0)

        for i in range(len(path) - 1):
            p0, p1 = path[i], path[i+1]
            seg_dir = p1[:2] - p0[:2]
            seg_len = np.linalg.norm(seg_dir)
            if seg_len < 0.01:
                continue
            seg_dir /= seg_len
            n = np.array([-seg_dir[1], seg_dir[0]])

            def make_deck_box(half_w, z_off, z_thick, color, alpha):
                corners_b = [
                    p0[:2] + n * half_w,
                    p1[:2] + n * half_w,
                    p1[:2] - n * half_w,
                    p0[:2] - n * half_w,
                ]
                z0_vals = [p0[2] + z_off, p1[2] + z_off, p1[2] + z_off, p0[2] + z_off]
                z1_vals = [z + z_thick for z in z0_vals]
                bottom = [(c[0], c[1], z) for c, z in zip(corners_b, z0_vals)]
                top_f = [(c[0], c[1], z) for c, z in zip(corners_b, z1_vals)]
                faces = [
                    bottom[::-1], top_f,
                    [bottom[0], bottom[1], top_f[1], top_f[0]],
                    [bottom[1], bottom[2], top_f[2], top_f[1]],
                    [bottom[2], bottom[3], top_f[3], top_f[2]],
                    [bottom[3], bottom[0], top_f[0], top_f[3]],
                ]
                pc = Poly3DCollection(faces, alpha=alpha, facecolor=color,
                                      edgecolor="#444444", linewidth=0.3)
                ax.add_collection3d(pc)

            make_deck_box(w/2, -thick, thick, BRIDGE_COLORS["deck"], 0.6)

            if sw_w > 0:
                road_half = w/2 - sw_w
                make_deck_box(w/2, 0, sw_h + 0.05, BRIDGE_COLORS["sidewalk"], 0.3)

            for pt in [p0, p1]:
                all_x.append(pt[0]); all_y.append(pt[1]); all_z.append(pt[2])
                all_z.append(pt[2] - thick)

        for pier in bridge.get("piers", []):
            station = pier["station_m"]
            total_len = sum(np.linalg.norm(path[i+1][:2] - path[i][:2])
                            for i in range(len(path)-1))
            t = station / total_len if total_len > 0 else 0
            cum = 0
            pier_pos = path[0].copy()
            for i in range(len(path)-1):
                seg_l = np.linalg.norm(path[i+1][:2] - path[i][:2])
                if cum + seg_l >= station:
                    local_t = (station - cum) / seg_l if seg_l > 0 else 0
                    pier_pos = path[i] + (path[i+1] - path[i]) * local_t
                    break
                cum += seg_l

            pw, pd = pier["width_m"], pier["depth_m"]
            faces = box_faces(
                pier_pos[0] - pw/2, pier_pos[1] - pd/2, pier["bottom_z"],
                pier_pos[0] + pw/2, pier_pos[1] + pd/2, pier["top_z"]
            )
            pc = Poly3DCollection(faces, alpha=0.7, facecolor=BRIDGE_COLORS["pier"],
                                  edgecolor="#333333", linewidth=0.4)
            ax.add_collection3d(pc)
            all_z.append(pier["bottom_z"])

        for abut in bridge.get("abutments", []):
            station = abut["station_m"]
            if station <= 0:
                apos = path[0].copy()
            else:
                apos = path[-1].copy()
            aw, ad = abut["width_m"], abut["depth_m"]
            faces = box_faces(
                apos[0] - aw/2, apos[1] - ad/2, abut["bottom_z"],
                apos[0] + aw/2, apos[1] + ad/2, abut["top_z"]
            )
            pc = Poly3DCollection(faces, alpha=0.7, facecolor=BRIDGE_COLORS["abutment"],
                                  edgecolor="#333333", linewidth=0.4)
            ax.add_collection3d(pc)
            all_z.append(abut["bottom_z"])

        railings = bridge.get("railings", {})
        rh = railings.get("height_m", 1.1)
        rt = railings.get("thickness_m", 0.15)
        for side in [1, -1]:
            for i in range(len(path) - 1):
                p0, p1 = path[i], path[i+1]
                seg_d = p1[:2] - p0[:2]
                seg_l = np.linalg.norm(seg_d)
                if seg_l < 0.01:
                    continue
                seg_d /= seg_l
                n_vec = np.array([-seg_d[1], seg_d[0]]) * side * (w/2)
                r0 = np.array([p0[0] + n_vec[0], p0[1] + n_vec[1], p0[2]])
                r1 = np.array([p1[0] + n_vec[0], p1[1] + n_vec[1], p1[2]])
                rn = np.array([-seg_d[1], seg_d[0]]) * side * rt/2

                corners_b = [
                    [r0[0]+rn[0], r0[1]+rn[1], r0[2]],
                    [r1[0]+rn[0], r1[1]+rn[1], r1[2]],
                    [r1[0]-rn[0], r1[1]-rn[1], r1[2]],
                    [r0[0]-rn[0], r0[1]-rn[1], r0[2]],
                ]
                corners_t = [[c[0], c[1], c[2]+rh] for c in corners_b]
                faces = [
                    corners_b[::-1], corners_t,
                    [corners_b[0], corners_b[1], corners_t[1], corners_t[0]],
                    [corners_b[1], corners_b[2], corners_t[2], corners_t[1]],
                    [corners_b[2], corners_b[3], corners_t[3], corners_t[2]],
                    [corners_b[3], corners_b[0], corners_t[0], corners_t[3]],
                ]
                pc = Poly3DCollection(faces, alpha=0.4, facecolor=BRIDGE_COLORS["railing"],
                                      edgecolor="#555555", linewidth=0.2)
                ax.add_collection3d(pc)

    if all_x:
        margin = 5
        ax.set_xlim(min(all_x) - margin, max(all_x) + margin)
        ax.set_ylim(min(all_y) - margin - 10, max(all_y) + margin + 10)
        ax.set_zlim(min(all_z) - 1, max(all_z) + 5)

    ax.set_xlabel("X (m)")
    ax.set_ylabel("Y (m)")
    ax.set_zlabel("Z (m)")
    ax.view_init(elev=22, azim=-60)

    legend_items = [
        patches.Patch(color=BRIDGE_COLORS["deck"], label="Deck"),
        patches.Patch(color=BRIDGE_COLORS["pier"], label="Pier"),
        patches.Patch(color=BRIDGE_COLORS["abutment"], label="Abutment"),
        patches.Patch(color=BRIDGE_COLORS["railing"], label="Railing"),
    ]
    ax.legend(handles=legend_items, loc="upper left", fontsize=7, framealpha=0.8)

    fig.savefig(output_path, dpi=180, bbox_inches="tight",
                facecolor="white", edgecolor="none")
    plt.close(fig)
    print(f"  Saved bridge: {output_path}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    samples_dir = os.path.join(SCRIPT_DIR, "samples")
    out_dir = os.path.join(SCRIPT_DIR, "output", "blockbox_preview")
    os.makedirs(out_dir, exist_ok=True)

    print("\n=== Block-box Visualization ===\n")

    factory_path = os.path.join(samples_dir, "floorplan_factory_sample.json")
    if os.path.exists(factory_path):
        with open(factory_path, encoding="utf-8") as f:
            factory = json.load(f)
        print("[Factory]")
        visualize_building_2d(factory, os.path.join(out_dir, "factory_2d_plan.png"))
        visualize_building_3d(factory, os.path.join(out_dir, "factory_3d_blockbox.png"))

    school_path = os.path.join(samples_dir, "floorplan_school_sample.json")
    if os.path.exists(school_path):
        with open(school_path, encoding="utf-8") as f:
            school = json.load(f)
        print("[School]")
        visualize_building_2d(school, os.path.join(out_dir, "school_2d_plan.png"))
        visualize_building_3d(school, os.path.join(out_dir, "school_3d_blockbox.png"))

    lshape_path = os.path.join(samples_dir, "floorplan_L_shape_sample.json")
    if os.path.exists(lshape_path):
        with open(lshape_path, encoding="utf-8") as f:
            lshape = json.load(f)
        print("[L-shape Hospital]")
        visualize_building_2d(lshape, os.path.join(out_dir, "lshape_2d_plan.png"))
        visualize_building_3d(lshape, os.path.join(out_dir, "lshape_3d_blockbox.png"))

    for gen_name in ["generated_hospital", "generated_school", "generated_warehouse"]:
        gen_path = os.path.join(samples_dir, f"{gen_name}.json")
        if os.path.exists(gen_path):
            with open(gen_path, encoding="utf-8") as f:
                gen_data = json.load(f)
            print(f"[{gen_name}]")
            visualize_building_2d(gen_data, os.path.join(out_dir, f"{gen_name}_2d.png"))
            visualize_building_3d(gen_data, os.path.join(out_dir, f"{gen_name}_3d.png"))

    bridge_path = os.path.join(samples_dir, "floorplan_bridge_sample.json")
    if os.path.exists(bridge_path):
        with open(bridge_path, encoding="utf-8") as f:
            bridge_data = json.load(f)
        print("[Bridge]")
        visualize_bridge_3d(bridge_data, os.path.join(out_dir, "bridge_3d_blockbox.png"))

    print(f"\nAll outputs: {out_dir}")
    print("=== Done ===\n")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        out_dir = os.path.join(SCRIPT_DIR, "output", "blockbox_preview")
        os.makedirs(out_dir, exist_ok=True)
        for fp in sys.argv[1:]:
            if os.path.exists(fp):
                with open(fp, encoding="utf-8") as f:
                    data = json.load(f)
                name = os.path.splitext(os.path.basename(fp))[0]
                print(f"[{name}]")
                if data.get("floors"):
                    visualize_building_2d(data, os.path.join(out_dir, f"{name}_2d.png"))
                    visualize_building_3d(data, os.path.join(out_dir, f"{name}_3d.png"))
                if data.get("bridges"):
                    visualize_bridge_3d(data, os.path.join(out_dir, f"{name}_bridge_3d.png"))
    else:
        main()
