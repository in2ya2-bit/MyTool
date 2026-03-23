"""
Floorplan Parser (F-1a + F-1b)

Converts DXF files or floor plan images into floorplan.json.

Usage:
  # DXF → floorplan.json
  python floorplan_parser.py --input blueprint.dxf --output floorplan.json \
      --source dxf --floor-height 3.2

  # Image → floorplan.json
  python floorplan_parser.py --input blueprint.png --output floorplan.json \
      --source image --ppm 50 --floor-height 3.2

  # Multi-floor images
  python floorplan_parser.py --input 1F.png 2F.png 3F.png --output floorplan.json \
      --source image --ppm 50 --floor-height 3.2 3.2 3.0
"""

import argparse
import json
import math
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone

from floorplan_postprocess import (
    snap_endpoints,
    split_walls_at_junctions,
    merge_collinear_segments,
    snap_to_orthogonal,
    detect_gaps,
    classify_and_attach_openings,
    detect_rooms,
    classify_wall_types,
    estimate_room_labels,
    auto_place_stairs,
    validate_floorplan,
    print_validation_report,
)


# ── DXF Helpers ───────────────────────────────────────────────────────────────

def _infer_thickness(layer_name: str) -> float:
    layer = layer_name.upper()
    if "EXT" in layer:
        return 0.25
    if "PART" in layer:
        return 0.12
    if "INT" in layer:
        return 0.15
    return 0.15


def _load_layer_mapping(path: str) -> dict:
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _layer_matches(layer_name: str, keywords: list) -> bool:
    upper = layer_name.upper()
    return any(kw.upper() in upper for kw in keywords)


def _classify_layer(layer_name: str, mapping: dict) -> str:
    """Return category: walls_exterior, walls_interior, doors, windows, columns, stairs, ignore, unknown."""
    for category, patterns in mapping.items():
        if category.startswith("_") or category in ("thickness_overrides", "unit", "scale_to_meter"):
            continue
        if isinstance(patterns, list):
            for p in patterns:
                if p.upper() in layer_name.upper() or layer_name.upper() in p.upper():
                    return category
    if _layer_matches(layer_name, ["WALL", "벽"]):
        if _layer_matches(layer_name, ["EXT", "외"]):
            return "walls_exterior"
        if _layer_matches(layer_name, ["PART", "경량"]):
            return "walls_partition"
        return "walls_interior"
    if _layer_matches(layer_name, ["DOOR", "문"]):
        return "doors"
    if _layer_matches(layer_name, ["WINDOW", "WIN", "GLAZ", "창"]):
        return "windows"
    if _layer_matches(layer_name, ["COLUMN", "COL", "기둥"]):
        return "columns"
    if _layer_matches(layer_name, ["STAIR", "계단"]):
        return "stairs"
    if _layer_matches(layer_name, ["ANNO", "DIMS", "TEXT", "DEFPOINTS", "HATCH", "치수", "문자"]):
        return "ignore"
    return "unknown"


def _arc_to_segments(center_x, center_y, radius, start_angle_deg, end_angle_deg, wall_counter, layer, thickness):
    """Convert a DXF ARC to line segments."""
    start_rad = math.radians(start_angle_deg)
    end_rad = math.radians(end_angle_deg)
    if end_rad <= start_rad:
        end_rad += 2 * math.pi
    arc_length = radius * (end_rad - start_rad)
    n_segs = max(4, int(arc_length / 1.0))
    walls = []
    for i in range(n_segs):
        a0 = start_rad + (end_rad - start_rad) * i / n_segs
        a1 = start_rad + (end_rad - start_rad) * (i + 1) / n_segs
        x0 = center_x + radius * math.cos(a0)
        y0 = center_y + radius * math.sin(a0)
        x1 = center_x + radius * math.cos(a1)
        y1 = center_y + radius * math.sin(a1)
        wall_counter += 1
        walls.append({
            "wall_id": f"w_{wall_counter:03d}",
            "start": [round(x0, 4), round(y0, 4)],
            "end": [round(x1, 4), round(y1, 4)],
            "thickness_m": thickness,
            "type": "exterior" if "EXT" in layer.upper() else "interior",
            "openings": [],
        })
    return walls, wall_counter


# ── DXF Parser (F-1a) ────────────────────────────────────────────────────────

def parse_dxf_floorplan(dxf_path: str, floor_index: int = 0,
                         floor_height: float = 3.2,
                         layer_mapping_path: str = "",
                         unit_scale: float = 1.0) -> dict:
    """Parse a DXF file into a single floor dict."""
    import ezdxf

    doc = ezdxf.readfile(dxf_path)
    msp = doc.modelspace()

    mapping = _load_layer_mapping(layer_mapping_path)
    thickness_overrides = mapping.get("thickness_overrides", {})

    if not unit_scale or unit_scale == 1.0:
        scale_val = mapping.get("scale_to_meter", 1.0)
        unit = mapping.get("unit", "meter")
        if unit == "millimeter":
            unit_scale = 0.001
        elif unit == "centimeter":
            unit_scale = 0.01
        else:
            unit_scale = scale_val

    walls = []
    columns = []
    openings_raw = []
    wall_counter = 0

    has_wall_layer = False
    for entity in msp:
        cat = _classify_layer(entity.dxf.layer, mapping)
        if cat.startswith("walls"):
            has_wall_layer = True
            break

    for entity in msp:
        layer = entity.dxf.layer
        cat = _classify_layer(layer, mapping)

        if cat == "ignore":
            continue

        is_wall = cat.startswith("walls") or (not has_wall_layer and cat == "unknown"
                                               and entity.dxftype() in ("LINE", "LWPOLYLINE"))

        if is_wall:
            wall_type = "exterior" if "exterior" in cat else (
                "partition" if "partition" in cat else "interior"
            )
            t = thickness_overrides.get(layer, _infer_thickness(layer))

            if entity.dxftype() == "LINE":
                wall_counter += 1
                walls.append({
                    "wall_id": f"w_{wall_counter:03d}",
                    "start": [round(entity.dxf.start.x * unit_scale, 4),
                              round(entity.dxf.start.y * unit_scale, 4)],
                    "end": [round(entity.dxf.end.x * unit_scale, 4),
                            round(entity.dxf.end.y * unit_scale, 4)],
                    "thickness_m": t,
                    "type": wall_type,
                    "openings": [],
                })

            elif entity.dxftype() == "LWPOLYLINE":
                points = [(round(p[0] * unit_scale, 4), round(p[1] * unit_scale, 4))
                          for p in entity.get_points(format="xy")]
                is_closed = entity.closed
                n_pts = len(points)
                rng = n_pts if is_closed else n_pts - 1
                for i in range(rng):
                    j = (i + 1) % n_pts
                    wall_counter += 1
                    walls.append({
                        "wall_id": f"w_{wall_counter:03d}",
                        "start": list(points[i]),
                        "end": list(points[j]),
                        "thickness_m": t,
                        "type": wall_type,
                        "openings": [],
                    })

            elif entity.dxftype() == "ARC":
                cx = entity.dxf.center.x * unit_scale
                cy = entity.dxf.center.y * unit_scale
                r = entity.dxf.radius * unit_scale
                arc_walls, wall_counter = _arc_to_segments(
                    cx, cy, r, entity.dxf.start_angle, entity.dxf.end_angle,
                    wall_counter, layer, t
                )
                walls.extend(arc_walls)

        elif cat == "doors" or cat == "windows":
            if entity.dxftype() == "INSERT":
                ix = entity.dxf.insert.x * unit_scale
                iy = entity.dxf.insert.y * unit_scale
                block_name = entity.dxf.name.upper()
                sx = getattr(entity.dxf, 'xscale', 1.0)

                otype = "door" if cat == "doors" else "window"
                est_width = abs(sx) * unit_scale if abs(sx) > 0.1 else (1.0 if otype == "door" else 1.5)

                openings_raw.append({
                    "type": otype,
                    "x": ix,
                    "y": iy,
                    "width_m": round(est_width, 2),
                    "height_m": 2.1 if otype == "door" else 1.5,
                    "sill_m": 0.0 if otype == "door" else 0.9,
                })

        elif cat == "columns":
            if entity.dxftype() == "CIRCLE":
                columns.append({
                    "column_id": f"col_{len(columns) + 1:03d}",
                    "center": [round(entity.dxf.center.x * unit_scale, 4),
                               round(entity.dxf.center.y * unit_scale, 4)],
                    "size_m": [round(entity.dxf.radius * 2 * unit_scale, 3)],
                    "shape": "circle",
                })
            elif entity.dxftype() == "INSERT":
                columns.append({
                    "column_id": f"col_{len(columns) + 1:03d}",
                    "center": [round(entity.dxf.insert.x * unit_scale, 4),
                               round(entity.dxf.insert.y * unit_scale, 4)],
                    "size_m": [0.4, 0.4],
                    "shape": "square",
                })

    _attach_openings_to_nearest_wall(walls, openings_raw)

    snap_count = snap_endpoints(walls, tolerance=0.1)
    if snap_count:
        print(f"    Snapped {snap_count} endpoints")

    walls = split_walls_at_junctions(walls, tolerance=0.1, id_prefix=f"f{floor_index}")
    print(f"    After T-junction split: {len(walls)} walls")

    rooms = detect_rooms(walls)
    classify_wall_types(walls, rooms)
    estimate_room_labels(rooms, walls)

    for i, room in enumerate(rooms):
        room["room_id"] = f"f{floor_index}_r_{i+1:03d}"

    print(f"    DXF parsed: {len(walls)} walls, {len(rooms)} rooms, "
          f"{len(columns)} columns, {sum(len(w['openings']) for w in walls)} openings")

    return {
        "floor_index": floor_index,
        "label": f"{floor_index + 1}F",
        "elevation_m": round(floor_index * floor_height, 2),
        "height_m": floor_height,
        "walls": walls,
        "rooms": rooms,
        "stairs": [],
        "columns": columns,
        "elevators": [],
    }


def _attach_openings_to_nearest_wall(walls, openings_raw):
    """Attach raw opening positions to nearest walls."""
    op_counter = 0
    for op in openings_raw:
        best_wall = None
        best_dist = float("inf")
        best_offset = 0

        for wall in walls:
            sx, sy = wall["start"]
            ex, ey = wall["end"]
            wlen = math.sqrt((ex - sx) ** 2 + (ey - sy) ** 2)
            if wlen < 0.01:
                continue
            dx, dy = (ex - sx) / wlen, (ey - sy) / wlen

            t_val = (op["x"] - sx) * dx + (op["y"] - sy) * dy
            t_val = max(0, min(wlen, t_val))
            proj_x = sx + dx * t_val
            proj_y = sy + dy * t_val
            d = math.sqrt((op["x"] - proj_x) ** 2 + (op["y"] - proj_y) ** 2)

            if d < best_dist:
                best_dist = d
                best_wall = wall
                best_offset = t_val

        if best_wall and best_dist < 1.0:
            op_counter += 1
            best_wall["openings"].append({
                "opening_id": f"op_{op_counter:03d}",
                "type": op["type"],
                "offset_m": round(best_offset - op["width_m"] / 2, 2),
                "width_m": op["width_m"],
                "height_m": op["height_m"],
                "sill_m": op["sill_m"],
            })


# ── Image Parser (F-1b) ──────────────────────────────────────────────────────

def detect_walls_from_image(image_path: str,
                             pixels_per_meter: float = 50.0,
                             wall_thickness_px: int = 3,
                             floor_index: int = 0,
                             floor_height: float = 3.2) -> dict:
    """Parse a floor plan image into a single floor dict."""
    import cv2
    import numpy as np

    img = cv2.imread(image_path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        print(f"  ERROR: Cannot read image: {image_path}")
        return None

    h, w = img.shape
    print(f"    Image: {w}x{h}px, PPM={pixels_per_meter}")

    blurred = cv2.GaussianBlur(img, (5, 5), 1.5)
    binary = cv2.adaptiveThreshold(
        blurred, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV, 11, 2
    )

    kernel_close = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    closed = cv2.morphologyEx(binary, cv2.MORPH_CLOSE, kernel_close, iterations=2)

    kernel_open = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
    cleaned = cv2.morphologyEx(closed, cv2.MORPH_OPEN, kernel_open, iterations=1)

    if hasattr(cv2, 'ximgproc'):
        thinned = cv2.ximgproc.thinning(cleaned)
    else:
        thinned = cleaned
        print("    Warning: cv2.ximgproc not available, skipping thinning")

    threshold = 50
    lines = None
    for attempt in range(3):
        lines = cv2.HoughLinesP(
            thinned, rho=1, theta=math.pi / 180,
            threshold=threshold, minLineLength=30, maxLineGap=10
        )
        if lines is not None and len(lines) <= 1000:
            break
        threshold += 50
        print(f"    Retry HoughLines with threshold={threshold}")

    if lines is None:
        print("  WARNING: No lines detected")
        return {
            "floor_index": floor_index, "label": f"{floor_index + 1}F",
            "elevation_m": round(floor_index * floor_height, 2), "height_m": floor_height,
            "walls": [], "rooms": [], "stairs": [], "columns": [], "elevators": [],
        }

    raw_segments = [(l[0][0], l[0][1], l[0][2], l[0][3]) for l in lines]
    print(f"    Raw lines: {len(raw_segments)}")

    merged = merge_collinear_segments(raw_segments, angle_tol=5.0, dist_tol=10)
    snapped = snap_to_orthogonal(merged, angle_tol=3.0)
    print(f"    After merge+snap: {len(snapped)}")

    ppm = pixels_per_meter
    walls = []
    for i, (x1, y1, x2, y2) in enumerate(snapped):
        walls.append({
            "wall_id": f"w_{i + 1:03d}",
            "start": [round(x1 / ppm, 3), round((h - y1) / ppm, 3)],
            "end": [round(x2 / ppm, 3), round((h - y2) / ppm, 3)],
            "thickness_m": round(wall_thickness_px / ppm, 3),
            "type": "exterior",
            "openings": [],
        })

    gaps = detect_gaps(snapped, max_gap=3.0 * ppm)
    scaled_gaps = []
    for g in gaps:
        scaled_gaps.append({
            "wall_idx": g["wall_idx"],
            "offset": g["offset"] / ppm,
            "width": g["width"] / ppm,
        })
    classify_and_attach_openings(walls, scaled_gaps)

    snap_endpoints(walls, tolerance=0.15)
    walls = split_walls_at_junctions(walls, tolerance=0.15, id_prefix=f"f{floor_index}")
    rooms = detect_rooms(walls)
    classify_wall_types(walls, rooms)
    estimate_room_labels(rooms, walls)

    for i, room in enumerate(rooms):
        room["room_id"] = f"f{floor_index}_r_{i+1:03d}"

    total_openings = sum(len(w["openings"]) for w in walls)
    print(f"    Result: {len(walls)} walls, {len(rooms)} rooms, {total_openings} openings")

    return {
        "floor_index": floor_index,
        "label": f"{floor_index + 1}F",
        "elevation_m": round(floor_index * floor_height, 2),
        "height_m": floor_height,
        "walls": walls,
        "rooms": rooms,
        "stairs": [],
        "columns": [],
        "elevators": [],
    }


# ── Assemble Full Floorplan ───────────────────────────────────────────────────

def assemble_floorplan(floors: list, building_name: str, source: str,
                        source_file: str) -> dict:
    if len(floors) >= 2:
        n = auto_place_stairs(floors)
        if n:
            print(f"\n  Auto-placed {n} stairwells across {len(floors)} floors")

    return {
        "version": "1.0",
        "building_name": building_name,
        "source": source,
        "source_file": source_file,
        "scale": {"unit": "meter", "pixels_per_meter": 0, "origin": [0.0, 0.0]},
        "floors": floors,
        "bridges": [],
        "metadata": {
            "author": "floorplan_parser.py",
            "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
    }


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Parse floor plans (DXF/Image) → floorplan.json")
    parser.add_argument("--input", "-i", nargs="+", required=True, help="Input file(s)")
    parser.add_argument("--output", "-o", required=True, help="Output JSON path")
    parser.add_argument("--source", "-s", required=True, choices=["dxf", "image"])
    parser.add_argument("--floor-height", type=float, nargs="+", default=[3.2])
    parser.add_argument("--ppm", type=float, default=50.0, help="Pixels per meter (image mode)")
    parser.add_argument("--wall-thickness-px", type=int, default=3, help="Wall thickness in px (image mode)")
    parser.add_argument("--layer-mapping", type=str, default="", help="Layer mapping JSON (DXF mode)")
    parser.add_argument("--name", type=str, default="", help="Building name")
    parser.add_argument("--validate", action="store_true", help="Run validation after parsing")

    args = parser.parse_args()

    print(f"\n=== Floorplan Parser ===")
    print(f"  Source : {args.source}")
    print(f"  Input  : {args.input}")
    print(f"  Output : {args.output}\n")

    floors = []
    for fi, input_path in enumerate(args.input):
        fh = args.floor_height[fi] if fi < len(args.floor_height) else args.floor_height[-1]
        print(f"  Floor {fi}: {input_path} (height={fh}m)")

        if args.source == "dxf":
            floor = parse_dxf_floorplan(
                input_path, floor_index=fi, floor_height=fh,
                layer_mapping_path=args.layer_mapping,
            )
        else:
            floor = detect_walls_from_image(
                input_path, pixels_per_meter=args.ppm,
                wall_thickness_px=args.wall_thickness_px,
                floor_index=fi, floor_height=fh,
            )

        if floor:
            floors.append(floor)

    name = args.name or os.path.splitext(os.path.basename(args.input[0]))[0]
    data = assemble_floorplan(floors, name, args.source, ", ".join(args.input))

    out_path = args.output
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    print(f"\n  Saved: {out_path}")

    if args.validate:
        print_validation_report(data, out_path)

    print(f"=== Done ===\n")


if __name__ == "__main__":
    main()
