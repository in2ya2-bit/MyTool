"""
Floorplan Post-processing (F-1c)

Shared pipeline for both DXF parser and Image detector:
  - Wall endpoint snapping
  - T-junction splitting
  - Room detection (minimum cycle in wall graph)
  - Wall type classification (exterior / interior / partition)
  - Opening classification from gaps
  - Room label estimation
  - Floorplan validation
"""

import math
from collections import defaultdict
from typing import Optional


# ── Geometry Helpers ──────────────────────────────────────────────────────────

def _dist(a, b):
    return math.sqrt((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2)


def _angle_deg(x1, y1, x2, y2):
    return math.degrees(math.atan2(y2 - y1, x2 - x1)) % 360


def _point_to_segment_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return _dist((px, py), (x1, y1))
    t = max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    proj_x = x1 + t * dx
    proj_y = y1 + t * dy
    return _dist((px, py), (proj_x, proj_y)), t


def _polygon_area(poly):
    n = len(poly)
    area = 0.0
    for i in range(n):
        j = (i + 1) % n
        area += poly[i][0] * poly[j][1]
        area -= poly[j][0] * poly[i][1]
    return area / 2.0


def _polygon_centroid(poly):
    n = len(poly)
    cx = sum(p[0] for p in poly) / n
    cy = sum(p[1] for p in poly) / n
    return cx, cy


# ── 1. Wall Endpoint Snapping ─────────────────────────────────────────────────

def snap_endpoints(walls: list, tolerance: float = 0.1) -> int:
    """Snap wall endpoints that are within tolerance to the same point. Returns snap count."""
    points = []
    for w in walls:
        points.append(w["start"])
        points.append(w["end"])

    snap_count = 0
    n = len(points)
    for i in range(n):
        for j in range(i + 1, n):
            if _dist(points[i], points[j]) < tolerance and points[i] != points[j]:
                avg = [(points[i][0] + points[j][0]) / 2.0,
                       (points[i][1] + points[j][1]) / 2.0]
                avg = [round(avg[0], 4), round(avg[1], 4)]
                points[i][0], points[i][1] = avg[0], avg[1]
                points[j][0], points[j][1] = avg[0], avg[1]
                snap_count += 1

    return snap_count


# ── 1b. T-Junction Wall Splitting ────────────────────────────────────────────

def split_walls_at_junctions(walls: list, tolerance: float = 0.1,
                              id_prefix: str = "") -> list:
    """Split walls where other wall endpoints touch mid-segment (T-junctions).

    This is critical for room detection: a wall (0,0)→(20,0) must be split
    at (10,0) if another wall has an endpoint there.
    """
    all_endpoints = set()
    for w in walls:
        all_endpoints.add((round(w["start"][0], 4), round(w["start"][1], 4)))
        all_endpoints.add((round(w["end"][0], 4), round(w["end"][1], 4)))

    pfx = f"{id_prefix}_" if id_prefix else ""
    new_walls = []
    wc = 0
    for w in walls:
        sx, sy = w["start"]
        ex, ey = w["end"]
        wlen = _dist(w["start"], w["end"])
        if wlen < 0.01:
            wc += 1
            w["wall_id"] = f"{pfx}w_{wc:03d}"
            new_walls.append(w)
            continue

        dx, dy = (ex - sx) / wlen, (ey - sy) / wlen

        split_ts = []
        for pt in all_endpoints:
            px, py = pt
            t_val = (px - sx) * dx + (py - sy) * dy
            if t_val < tolerance or t_val > wlen - tolerance:
                continue
            proj_x = sx + dx * t_val
            proj_y = sy + dy * t_val
            perp = _dist((px, py), (proj_x, proj_y))
            if perp < tolerance:
                split_ts.append(t_val)

        if not split_ts:
            wc += 1
            w["wall_id"] = f"{pfx}w_{wc:03d}"
            new_walls.append(w)
            continue

        split_ts.sort()
        prev_pt = [sx, sy]
        for t_val in split_ts:
            split_pt = [round(sx + dx * t_val, 4), round(sy + dy * t_val, 4)]
            wc += 1
            new_walls.append({
                "wall_id": f"{pfx}w_{wc:03d}",
                "start": list(prev_pt),
                "end": list(split_pt),
                "thickness_m": w["thickness_m"],
                "type": w["type"],
                "openings": [],
            })
            prev_pt = split_pt

        wc += 1
        new_walls.append({
            "wall_id": f"{pfx}w_{wc:03d}",
            "start": list(prev_pt),
            "end": [ex, ey],
            "thickness_m": w["thickness_m"],
            "type": w["type"],
            "openings": [],
        })

        for op in w.get("openings", []):
            best_w = None
            best_d = float("inf")
            op_mid = op["offset_m"] + op["width_m"] / 2
            op_pt = (sx + dx * op_mid, sy + dy * op_mid)
            for nw in new_walls:
                mid = ((nw["start"][0] + nw["end"][0]) / 2,
                       (nw["start"][1] + nw["end"][1]) / 2)
                d = _dist(op_pt, mid)
                if d < best_d:
                    best_d = d
                    best_w = nw
            if best_w:
                nw_s = best_w["start"]
                nw_len = _dist(nw_s, best_w["end"])
                new_offset = (op_pt[0] - nw_s[0]) * dx + (op_pt[1] - nw_s[1]) * dy - op["width_m"] / 2
                op_copy = dict(op)
                op_copy["offset_m"] = round(max(0, new_offset), 2)
                best_w["openings"].append(op_copy)

    return new_walls


# ── 2. Collinear Segment Merging ──────────────────────────────────────────────

def merge_collinear_segments(segments: list, angle_tol: float = 5.0,
                              dist_tol: float = 0.1) -> list:
    """Merge collinear segments that are close together."""
    if not segments:
        return []

    used = [False] * len(segments)
    merged = []

    for i in range(len(segments)):
        if used[i]:
            continue
        x1, y1, x2, y2 = segments[i]
        a1 = _angle_deg(x1, y1, x2, y2)

        group_pts = [(x1, y1), (x2, y2)]
        used[i] = True

        for j in range(i + 1, len(segments)):
            if used[j]:
                continue
            x3, y3, x4, y4 = segments[j]
            a2 = _angle_deg(x3, y3, x4, y4)

            angle_diff = min(abs(a1 - a2), 360 - abs(a1 - a2))
            if angle_diff > angle_tol and abs(angle_diff - 180) > angle_tol:
                continue

            mid = ((x3 + x4) / 2, (y3 + y4) / 2)
            seg_len = _dist((x1, y1), (x2, y2))
            if seg_len < 0.01:
                continue
            result = _point_to_segment_dist(mid[0], mid[1], x1, y1, x2, y2)
            perp_dist = result[0] if isinstance(result, tuple) else result

            if perp_dist < dist_tol:
                group_pts.extend([(x3, y3), (x4, y4)])
                used[j] = True

        if len(group_pts) >= 2:
            dx = x2 - x1
            dy = y2 - y1
            length = math.sqrt(dx * dx + dy * dy)
            if length < 0.01:
                merged.append(segments[i])
                continue
            ux, uy = dx / length, dy / length
            projections = [(p[0] * ux + p[1] * uy, p) for p in group_pts]
            projections.sort(key=lambda x: x[0])
            p_min = projections[0][1]
            p_max = projections[-1][1]
            merged.append((p_min[0], p_min[1], p_max[0], p_max[1]))

    return merged


# ── 3. Orthogonal Snapping ────────────────────────────────────────────────────

def snap_to_orthogonal(segments: list, angle_tol: float = 3.0) -> list:
    """Snap near-horizontal/vertical segments to exact 0/90 degrees."""
    result = []
    for x1, y1, x2, y2 in segments:
        angle = _angle_deg(x1, y1, x2, y2)
        if min(angle % 180, 180 - angle % 180) < angle_tol:
            avg_y = (y1 + y2) / 2.0
            result.append((x1, avg_y, x2, avg_y))
        elif min(abs(angle - 90), abs(angle - 270)) < angle_tol:
            avg_x = (x1 + x2) / 2.0
            result.append((avg_x, y1, avg_x, y2))
        else:
            result.append((x1, y1, x2, y2))
    return result


# ── 4. Gap Detection (for Image mode) ────────────────────────────────────────

def detect_gaps(segments: list, max_gap: float = 3.0) -> list:
    """Detect gaps between collinear segments on the same line."""
    gaps = []
    n = len(segments)
    for i in range(n):
        x1, y1, x2, y2 = segments[i]
        a1 = _angle_deg(x1, y1, x2, y2)
        seg_len_i = _dist((x1, y1), (x2, y2))
        if seg_len_i < 0.01:
            continue
        dx, dy = (x2 - x1) / seg_len_i, (y2 - y1) / seg_len_i

        for j in range(i + 1, n):
            x3, y3, x4, y4 = segments[j]
            a2 = _angle_deg(x3, y3, x4, y4)
            angle_diff = min(abs(a1 - a2), 360 - abs(a1 - a2))
            if angle_diff > 5 and abs(angle_diff - 180) > 5:
                continue

            mid34 = ((x3 + x4) / 2, (y3 + y4) / 2)
            result = _point_to_segment_dist(mid34[0], mid34[1], x1, y1, x2, y2)
            perp = result[0] if isinstance(result, tuple) else result
            if perp > 0.3:
                continue

            endpoints = [
                ((x1, y1), (x3, y3)), ((x1, y1), (x4, y4)),
                ((x2, y2), (x3, y3)), ((x2, y2), (x4, y4)),
            ]
            for pa, pb in endpoints:
                gap_w = _dist(pa, pb)
                if 0.3 < gap_w <= max_gap:
                    offset_along = min(
                        _dist((x1, y1), pa), _dist((x1, y1), pb)
                    )
                    gaps.append({
                        "wall_idx": i,
                        "offset": offset_along,
                        "width": gap_w,
                    })
    return gaps


def classify_and_attach_openings(walls: list, gaps: list, counter_start: int = 0):
    """Classify gaps as doors/windows and attach to walls."""
    oc = counter_start
    for gap in gaps:
        idx = gap["wall_idx"]
        if idx >= len(walls):
            continue
        w_m = gap["width"]
        oc += 1
        if 0.7 <= w_m <= 1.5:
            otype = "door"
        elif 0.5 <= w_m <= 3.0:
            otype = "window"
        else:
            continue
        walls[idx]["openings"].append({
            "opening_id": f"op_{oc:03d}",
            "type": otype,
            "offset_m": round(gap["offset"], 2),
            "width_m": round(w_m, 2),
            "height_m": 2.1 if otype == "door" else 1.5,
            "sill_m": 0.0 if otype == "door" else 0.9,
        })


# ── 5. Room Detection ────────────────────────────────────────────────────────

def _build_wall_graph(walls: list, snap_tol: float = 0.05):
    """Build adjacency graph from walls. Nodes = unique points, Edges = walls."""
    point_map = {}
    counter = 0

    def get_node(pt):
        nonlocal counter
        for key, idx in point_map.items():
            if _dist(pt, key) < snap_tol:
                return idx
        node_id = counter
        point_map[(round(pt[0], 4), round(pt[1], 4))] = node_id
        counter += 1
        return node_id

    adj = defaultdict(list)
    node_coords = {}
    edges = []

    for w in walls:
        s = get_node(w["start"])
        e = get_node(w["end"])
        if s == e:
            continue
        adj[s].append(e)
        adj[e].append(s)
        edges.append((s, e, w))

    for pt, idx in point_map.items():
        node_coords[idx] = pt

    return adj, node_coords, edges


def detect_rooms(walls: list, min_area: float = 1.0, max_area: float = 500.0) -> list:
    """Detect rooms as minimum cycles in the wall graph."""
    adj, coords, edges = _build_wall_graph(walls)

    if not coords:
        return []

    def angle_from(node, neighbor):
        nx, ny = coords[neighbor]
        ox, oy = coords[node]
        return math.atan2(ny - oy, nx - ox)

    visited_edges = set()
    polygons = []

    for start_node in adj:
        for first_neighbor in adj[start_node]:
            edge_key = (start_node, first_neighbor)
            if edge_key in visited_edges:
                continue

            path = [start_node, first_neighbor]
            visited_edges.add(edge_key)
            current = first_neighbor
            prev = start_node

            for _ in range(200):
                neighbors = adj.get(current, [])
                if not neighbors:
                    break

                incoming_angle = angle_from(current, prev)

                candidates = []
                for nb in neighbors:
                    if nb == prev and len(neighbors) > 1:
                        continue
                    out_angle = angle_from(current, nb)
                    turn = (out_angle - incoming_angle + math.pi) % (2 * math.pi) - math.pi
                    candidates.append((turn, nb))

                if not candidates:
                    break

                candidates.sort(key=lambda x: x[0])
                _, next_node = candidates[0]

                visited_edges.add((current, next_node))

                if next_node == start_node:
                    poly = [list(coords[n]) for n in path]
                    area = _polygon_area(poly)
                    abs_area = abs(area)
                    if min_area <= abs_area <= max_area:
                        if area < 0:
                            poly.reverse()
                        polygons.append(poly)
                    break

                if next_node in path:
                    break

                path.append(next_node)
                prev = current
                current = next_node

    seen = set()
    unique_rooms = []
    for poly in polygons:
        key = tuple(sorted((round(p[0], 2), round(p[1], 2)) for p in poly))
        if key not in seen:
            seen.add(key)
            unique_rooms.append(poly)

    rooms = []
    for i, poly in enumerate(unique_rooms):
        area = abs(_polygon_area(poly))
        rooms.append({
            "room_id": f"r_{i + 1:03d}",
            "label": "",
            "polygon": [[round(p[0], 3), round(p[1], 3)] for p in poly],
            "area_m2": round(area, 1),
            "usage": "other",
        })

    return rooms


# ── 6. Wall Type Classification ───────────────────────────────────────────────

def classify_wall_types(walls: list, rooms: list):
    """Reclassify wall types based on adjacent room count."""
    for wall in walls:
        sx, sy = wall["start"]
        ex, ey = wall["end"]
        mx, my = (sx + ex) / 2.0, (sy + ey) / 2.0
        length = _dist(wall["start"], wall["end"])
        if length < 0.01:
            continue
        nx = -(ey - sy) / length
        ny = (ex - sx) / length

        offset = 0.3
        p_left = (mx + nx * offset, my + ny * offset)
        p_right = (mx - nx * offset, my - ny * offset)

        adj_count = 0
        for room in rooms:
            poly = room["polygon"]
            if _point_in_polygon(p_left, poly):
                adj_count += 1
            if _point_in_polygon(p_right, poly):
                adj_count += 1

        t = wall["thickness_m"]
        if adj_count <= 1:
            wall["type"] = "exterior"
        elif adj_count == 2:
            wall["type"] = "interior" if t >= 0.12 else "partition"


def _point_in_polygon(point, polygon):
    x, y = point
    n = len(polygon)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = polygon[i]
        xj, yj = polygon[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi):
            inside = not inside
        j = i
    return inside


# ── 7. Room Label Estimation ──────────────────────────────────────────────────

def estimate_room_labels(rooms: list, walls: list):
    """Auto-estimate room labels based on area, doors, and shape."""
    for room in rooms:
        area = room["area_m2"]
        poly = room["polygon"]

        door_count = 0
        for wall in walls:
            for op in wall.get("openings", []):
                if op["type"] == "door":
                    door_count += 1

        xs = [p[0] for p in poly]
        ys = [p[1] for p in poly]
        w = max(xs) - min(xs)
        h = max(ys) - min(ys)
        aspect = max(w, h) / (min(w, h) + 0.01)

        if room["label"]:
            continue

        if aspect > 3.0 and min(w, h) < 3.0:
            room["label"] = "Corridor"
            room["usage"] = "corridor"
        elif area < 4:
            room["label"] = "Utility"
            room["usage"] = "utility"
        elif area <= 15:
            room["label"] = "Office"
            room["usage"] = "office"
        elif area <= 50:
            room["label"] = "Room"
            room["usage"] = "other"
        else:
            room["label"] = "Hall"
            room["usage"] = "lobby"


# ── 8. Auto Stair Placement ───────────────────────────────────────────────────

def auto_place_stairs(floors: list, min_floors: int = 2):
    """Automatically add stairs to multi-floor buildings that have none.

    Places 2 stairwells per floor: one near each end of the building,
    positioned inside the largest rooms or at building corners.
    """
    if len(floors) < min_floors:
        return 0

    has_stairs = any(len(f.get("stairs", [])) > 0 for f in floors)
    if has_stairs:
        return 0

    all_walls = []
    for f in floors:
        all_walls.extend(f.get("walls", []))

    ext_pts = []
    for w in all_walls:
        if w.get("type") == "exterior":
            ext_pts.append(w["start"])
            ext_pts.append(w["end"])

    if not ext_pts:
        for w in all_walls:
            ext_pts.append(w["start"])
            ext_pts.append(w["end"])

    if len(ext_pts) < 2:
        return 0

    xs = [p[0] for p in ext_pts]
    ys = [p[1] for p in ext_pts]
    bldg_min_x, bldg_max_x = min(xs), max(xs)
    bldg_min_y, bldg_max_y = min(ys), max(ys)
    bldg_w = bldg_max_x - bldg_min_x
    bldg_d = bldg_max_y - bldg_min_y

    stair_w = min(2.5, bldg_w * 0.1)
    stair_d = min(3.0, bldg_d * 0.2)
    margin = 1.0

    stair_positions = [
        (round(bldg_min_x + margin + stair_w / 2, 2),
         round(bldg_min_y + margin + stair_d / 2, 2)),
        (round(bldg_max_x - margin - stair_w / 2, 2),
         round(bldg_max_y - margin - stair_d / 2, 2)),
    ]

    if bldg_w < 6 or bldg_d < 6:
        stair_positions = [stair_positions[0]]

    stair_count = 0
    for fi, floor in enumerate(floors):
        if "stairs" not in floor:
            floor["stairs"] = []

        for si, (sx, sy) in enumerate(stair_positions):
            stair_count += 1
            floor["stairs"].append({
                "stair_id": f"st_auto_{fi}_{si+1:02d}",
                "location": [sx, sy],
                "size_m": [round(stair_w, 2), round(stair_d, 2)],
                "type": "180_turn" if stair_d > 2.5 else "straight",
            })

    return stair_count


# ── 9. Validation ─────────────────────────────────────────────────────────────

def validate_floorplan(data: dict) -> list:
    """Validate floorplan.json and return list of issues."""
    issues = []

    if "version" not in data:
        issues.append(("ERROR", "V-1", "Missing 'version' field"))

    all_wall_ids = set()
    for floor in data.get("floors", []):
        for wall in floor.get("walls", []):
            wid = wall["wall_id"]
            if wid in all_wall_ids:
                issues.append(("ERROR", "V-2", f"Duplicate wall_id: {wid}"))
            all_wall_ids.add(wid)

            s, e = wall["start"], wall["end"]
            if _dist(s, e) < 0.01:
                issues.append(("ERROR", "V-3", f"Zero-length wall: {wid}"))

            if wall.get("thickness_m", 0) <= 0:
                issues.append(("ERROR", "V-4", f"Invalid thickness: {wid}"))

            wlen = _dist(s, e)
            for op in wall.get("openings", []):
                if op["offset_m"] + op["width_m"] > wlen + 0.01:
                    issues.append(("WARNING", "V-5",
                                   f"Opening {op['opening_id']} exceeds wall {wid}"))

        for room in floor.get("rooms", []):
            poly = room["polygon"]
            if len(poly) < 3:
                issues.append(("WARNING", "V-7", f"Room {room['room_id']} has < 3 points"))
            area = abs(_polygon_area(poly))
            if area < 0.01:
                issues.append(("WARNING", "V-11", f"Room {room['room_id']} area ≈ 0"))

    for bridge in data.get("bridges", []):
        if len(bridge.get("path", [])) < 2:
            issues.append(("ERROR", "V-12", f"Bridge {bridge.get('bridge_id')} path < 2 points"))

    return issues


def print_validation_report(data: dict, filepath: str = ""):
    """Print human-readable validation report."""
    issues = validate_floorplan(data)

    total_walls = sum(len(f.get("walls", [])) for f in data.get("floors", []))
    total_rooms = sum(len(f.get("rooms", [])) for f in data.get("floors", []))
    total_openings = sum(
        len(w.get("openings", []))
        for f in data.get("floors", []) for w in f.get("walls", [])
    )

    errors = [i for i in issues if i[0] == "ERROR"]
    warnings = [i for i in issues if i[0] == "WARNING"]
    infos = [i for i in issues if i[0] == "INFO"]

    print(f"\n=== Floorplan Validation Report ===")
    print(f"  File: {filepath or '(unknown)'}")
    print(f"  Version: {data.get('version', '?')}")
    print(f"  Floors: {len(data.get('floors', []))}")
    print(f"  Walls: {total_walls}  Rooms: {total_rooms}  Openings: {total_openings}")
    print(f"  Errors: {len(errors)}  Warnings: {len(warnings)}  Info: {len(infos)}")
    for level, code, msg in issues:
        print(f"    [{level}] {code}: {msg}")
    result = "PASS" if not errors else "FAIL"
    print(f"  Result: {result}")
    print(f"===\n")
    return len(errors) == 0
