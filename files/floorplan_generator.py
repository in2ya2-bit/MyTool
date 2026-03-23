"""
Parameter-based floorplan.json generator (Track C).

Generates building floor plans from simple settings — no DXF/image needed.
Output is floorplan.json compatible with blender_blockbox.py and visualize_blockbox.py.

Usage:
    python floorplan_generator.py --width 20 --depth 12 --floors 3 --rooms 4 \
        --windows --doors every_room --output building.json
"""
import argparse
import json
import math
import os
import random
import sys
from datetime import datetime, timezone
from typing import Any


# ── defaults ──────────────────────────────────────────────────────────────────

DEFAULTS = {
    "floor_height": 3.5,
    "corridor_type": "single",
    "corridor_width": 3.0,
    "building_shape": "rectangle",
    "ext_wall_t": 0.25,
    "int_wall_t": 0.15,
    "part_wall_t": 0.12,
    "window_interval": 3.0,
    "window_width": 1.5,
    "window_height": 1.5,
    "window_sill": 0.9,
    "door_width": 1.0,
    "door_height": 2.1,
    "main_entrance_width": 2.0,
    "stair_location": "end",
}


# ── outline generators ────────────────────────────────────────────────────────

def _rect_outline(w: float, d: float) -> list[list[float]]:
    return [[0, 0], [w, 0], [w, d], [0, d]]


def _L_outline(w: float, d: float) -> list[list[float]]:
    """L-shape: main wing full width × 60% depth, secondary wing 50% width × full depth."""
    mx = w
    my = d * 0.6
    sx = w * 0.5
    return [[0, 0], [mx, 0], [mx, my], [sx, my], [sx, d], [0, d]]


def _T_outline(w: float, d: float) -> list[list[float]]:
    """T-shape: base full width × 60% depth, top center protrusion 40% width × full depth."""
    bx = w
    by = d * 0.6
    pw = w * 0.4
    px0 = (w - pw) / 2
    px1 = px0 + pw
    return [[0, 0], [bx, 0], [bx, by], [px1, by], [px1, d], [px0, d], [px0, by], [0, by]]


def _U_outline(w: float, d: float) -> list[list[float]]:
    """U-shape: base full width × 50% depth, two wings 30% width × full depth."""
    by = d * 0.5
    lw = w * 0.3
    rw = w * 0.3
    return [
        [0, 0], [w, 0], [w, d], [w - rw, d], [w - rw, by],
        [lw, by], [lw, d], [0, d]
    ]


OUTLINE_FN = {
    "rectangle": _rect_outline,
    "L": _L_outline,
    "T": _T_outline,
    "U": _U_outline,
}


# ── wall / opening helpers ────────────────────────────────────────────────────

_wall_counter = 0
_opening_counter = 0
_room_counter = 0
_stair_counter = 0


def _reset_counters():
    global _wall_counter, _opening_counter, _room_counter, _stair_counter
    _wall_counter = 0
    _opening_counter = 0
    _room_counter = 0
    _stair_counter = 0


def _next_wall_id() -> str:
    global _wall_counter
    _wall_counter += 1
    return f"w_{_wall_counter:03d}"


def _next_opening_id() -> str:
    global _opening_counter
    _opening_counter += 1
    return f"op_{_opening_counter:03d}"


def _next_room_id() -> str:
    global _room_counter
    _room_counter += 1
    return f"r_{_room_counter:03d}"


def _next_stair_id() -> str:
    global _stair_counter
    _stair_counter += 1
    return f"st_{_stair_counter:03d}"


def _make_wall(start, end, thickness, wtype, openings=None) -> dict:
    return {
        "wall_id": _next_wall_id(),
        "start": list(start),
        "end": list(end),
        "thickness_m": thickness,
        "type": wtype,
        "openings": openings or [],
    }


def _wall_length(wall: dict) -> float:
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    return math.sqrt((ex - sx) ** 2 + (ey - sy) ** 2)


def _place_windows_on_wall(wall: dict, cfg: dict) -> None:
    length = _wall_length(wall)
    interval = cfg["window_interval"]
    ww = cfg["window_width"]
    margin = 1.0
    offset = margin
    while offset + ww <= length - margin:
        wall["openings"].append({
            "opening_id": _next_opening_id(),
            "type": "window",
            "offset_m": round(offset, 2),
            "width_m": ww,
            "height_m": cfg["window_height"],
            "sill_m": cfg["window_sill"],
        })
        offset += interval


def _place_door_center(wall: dict, cfg: dict, door_type="door", width=None) -> None:
    length = _wall_length(wall)
    dw = width or cfg["door_width"]
    if length < dw + 0.5:
        return
    offset = (length - dw) / 2.0
    wall["openings"].append({
        "opening_id": _next_opening_id(),
        "type": door_type,
        "offset_m": round(offset, 2),
        "width_m": dw,
        "height_m": cfg["door_height"],
        "sill_m": 0.0,
    })


def _polygon_area(poly: list) -> float:
    n = len(poly)
    area = 0.0
    for i in range(n):
        j = (i + 1) % n
        area += poly[i][0] * poly[j][1]
        area -= poly[j][0] * poly[i][1]
    return abs(area) / 2.0


# ── single-floor generation (rectangle + single corridor) ────────────────────

def _generate_floor_rect_single(
    floor_idx: int,
    w: float,
    d: float,
    num_rooms: int,
    cfg: dict,
    is_ground: bool,
    is_top: bool,
    add_stairs: bool,
    stair_loc: str,
    rng: random.Random,
) -> dict:
    """Generate one floor for a rectangular building with single corridor."""
    fh = cfg["floor_height"]
    elev = floor_idx * fh
    cw = cfg["corridor_width"]
    et = cfg["ext_wall_t"]
    it = cfg["int_wall_t"]
    pt = cfg["part_wall_t"]

    walls = []
    rooms = []
    stairs = []

    # exterior walls
    w_south = _make_wall([0, 0], [w, 0], et, "exterior")
    w_east = _make_wall([w, 0], [w, d], et, "exterior")
    w_north = _make_wall([w, d], [0, d], et, "exterior")
    w_west = _make_wall([0, d], [0, 0], et, "exterior")

    if cfg.get("has_windows"):
        _place_windows_on_wall(w_south, cfg)
        _place_windows_on_wall(w_east, cfg)
        _place_windows_on_wall(w_north, cfg)
        _place_windows_on_wall(w_west, cfg)

    if is_ground:
        _place_door_center(w_south, cfg, width=cfg["main_entrance_width"])

    walls.extend([w_south, w_east, w_north, w_west])

    # corridor wall (interior, separating corridor from rooms)
    corridor_y = cw
    w_corridor = _make_wall([0, corridor_y], [w, corridor_y], it, "interior")
    walls.append(w_corridor)

    # corridor room
    rooms.append({
        "room_id": _next_room_id(),
        "label": "Corridor",
        "polygon": [[0, 0], [w, 0], [w, corridor_y], [0, corridor_y]],
        "area_m2": round(w * cw, 1),
        "usage": "corridor",
    })

    # determine stair slots
    stair_slots = set()
    if add_stairs and cfg.get("num_floors", 1) >= 2:
        if stair_loc == "end":
            stair_slots.add(num_rooms - 1)
        elif stair_loc == "center":
            stair_slots.add(num_rooms // 2)
        elif stair_loc == "both_ends":
            stair_slots.add(0)
            stair_slots.add(num_rooms - 1)

    effective_rooms = max(1, num_rooms)
    room_width = w / effective_rooms

    MIN_ROOM_W = 3.0
    if room_width < MIN_ROOM_W and effective_rooms > 1:
        effective_rooms = max(1, int(w / MIN_ROOM_W))
        room_width = w / effective_rooms
        print(f"  Warning: room width too small, reduced to {effective_rooms} rooms")

    room_depth = d - corridor_y

    for i in range(effective_rooms):
        x0 = i * room_width
        x1 = (i + 1) * room_width
        if i == effective_rooms - 1:
            x1 = w

        if i > 0:
            part = _make_wall([x0, corridor_y], [x0, d], pt, "partition")
            walls.append(part)

        is_stair = i in stair_slots
        label = f"Stairwell {i+1}" if is_stair else ""
        usage = "stairwell" if is_stair else "office"

        if not is_stair and cfg.get("room_labels"):
            label_idx = i - len([s for s in stair_slots if s < i])
            if label_idx < len(cfg["room_labels"]):
                label = cfg["room_labels"][label_idx]

        if not label:
            label = f"Room {i+1}"

        room_poly = [[x0, corridor_y], [x1, corridor_y], [x1, d], [x0, d]]
        rooms.append({
            "room_id": _next_room_id(),
            "label": label,
            "polygon": room_poly,
            "area_m2": round(room_width * room_depth, 1),
            "usage": usage,
        })

        # door from room to corridor
        door_placement = cfg.get("door_placement", "every_room")
        if door_placement == "every_room" or is_stair:
            door_wall = _make_wall([x0, corridor_y], [x1, corridor_y], it, "interior")
            _place_door_center(door_wall, cfg)
            if door_wall["openings"]:
                for op in door_wall["openings"]:
                    w_corridor["openings"].append(op.copy())

        if is_stair:
            sw = min(room_width * 0.8, 2.5)
            sd = min(room_depth * 0.6, 3.0)
            stairs.append({
                "stair_id": _next_stair_id(),
                "location": [round((x0 + x1) / 2, 2), round(corridor_y + room_depth / 2, 2)],
                "size_m": [round(sw, 2), round(sd, 2)],
                "type": "180_turn" if room_depth > 5 else "straight",
                "connects_floors": [floor_idx, floor_idx + 1] if not is_top else [floor_idx - 1, floor_idx],
                "direction_deg": 90,
            })

    return {
        "floor_index": floor_idx,
        "label": f"{floor_idx + 1}F",
        "elevation_m": round(elev, 2),
        "height_m": fh,
        "walls": walls,
        "rooms": rooms,
        "stairs": stairs,
        "columns": [],
        "elevators": [],
    }


def _generate_floor_rect_double(
    floor_idx: int,
    w: float,
    d: float,
    num_rooms: int,
    cfg: dict,
    is_ground: bool,
    is_top: bool,
    add_stairs: bool,
    stair_loc: str,
    rng: random.Random,
) -> dict:
    """Rectangular building with double-loaded corridor (rooms on both sides)."""
    fh = cfg["floor_height"]
    elev = floor_idx * fh
    cw = cfg["corridor_width"]
    et = cfg["ext_wall_t"]
    it = cfg["int_wall_t"]
    pt = cfg["part_wall_t"]

    walls = []
    rooms = []
    stairs = []

    cy_low = (d - cw) / 2.0
    cy_high = cy_low + cw

    w_south = _make_wall([0, 0], [w, 0], et, "exterior")
    w_east = _make_wall([w, 0], [w, d], et, "exterior")
    w_north = _make_wall([w, d], [0, d], et, "exterior")
    w_west = _make_wall([0, d], [0, 0], et, "exterior")

    if cfg.get("has_windows"):
        _place_windows_on_wall(w_south, cfg)
        _place_windows_on_wall(w_east, cfg)
        _place_windows_on_wall(w_north, cfg)
        _place_windows_on_wall(w_west, cfg)

    if is_ground:
        _place_door_center(w_south, cfg, width=cfg["main_entrance_width"])

    walls.extend([w_south, w_east, w_north, w_west])

    w_corr_south = _make_wall([0, cy_low], [w, cy_low], it, "interior")
    w_corr_north = _make_wall([0, cy_high], [w, cy_high], it, "interior")
    walls.extend([w_corr_south, w_corr_north])

    rooms.append({
        "room_id": _next_room_id(),
        "label": "Corridor",
        "polygon": [[0, cy_low], [w, cy_low], [w, cy_high], [0, cy_high]],
        "area_m2": round(w * cw, 1),
        "usage": "corridor",
    })

    rooms_per_side = max(1, num_rooms // 2)
    room_w = w / rooms_per_side

    stair_slots = set()
    if add_stairs and cfg.get("num_floors", 1) >= 2:
        if stair_loc == "end":
            stair_slots.add(("south", rooms_per_side - 1))
        elif stair_loc == "center":
            stair_slots.add(("south", rooms_per_side // 2))
        elif stair_loc == "both_ends":
            stair_slots.add(("south", 0))
            stair_slots.add(("north", rooms_per_side - 1))

    for side, y0, y1, corr_wall in [
        ("south", 0, cy_low, w_corr_south),
        ("north", cy_high, d, w_corr_north),
    ]:
        rdepth = y1 - y0
        for i in range(rooms_per_side):
            x0 = i * room_w
            x1 = (i + 1) * room_w if i < rooms_per_side - 1 else w
            actual_w = x1 - x0

            if i > 0:
                walls.append(_make_wall([x0, y0], [x0, y1], pt, "partition"))

            is_stair = (side, i) in stair_slots
            label = f"Stairwell" if is_stair else f"Room {side[0].upper()}{i+1}"
            usage = "stairwell" if is_stair else "office"

            rooms.append({
                "room_id": _next_room_id(),
                "label": label,
                "polygon": [[x0, y0], [x1, y0], [x1, y1], [x0, y1]],
                "area_m2": round(actual_w * rdepth, 1),
                "usage": usage,
            })

            door_placement = cfg.get("door_placement", "every_room")
            if door_placement == "every_room" or is_stair:
                temp = _make_wall([x0, y0], [x1, y0], it, "interior")
                _place_door_center(temp, cfg)
                for op in temp["openings"]:
                    corr_wall["openings"].append(op.copy())

            if is_stair:
                sw = min(actual_w * 0.8, 2.5)
                sd = min(rdepth * 0.6, 3.0)
                stairs.append({
                    "stair_id": _next_stair_id(),
                    "location": [round((x0 + x1) / 2, 2), round((y0 + y1) / 2, 2)],
                    "size_m": [round(sw, 2), round(sd, 2)],
                    "type": "180_turn" if rdepth > 5 else "straight",
                    "connects_floors": [floor_idx, floor_idx + 1] if not is_top else [floor_idx - 1, floor_idx],
                    "direction_deg": 90,
                })

    return {
        "floor_index": floor_idx,
        "label": f"{floor_idx + 1}F",
        "elevation_m": round(elev, 2),
        "height_m": fh,
        "walls": walls,
        "rooms": rooms,
        "stairs": stairs,
        "columns": [],
        "elevators": [],
    }


def _generate_floor_no_corridor(
    floor_idx: int,
    w: float,
    d: float,
    num_rooms: int,
    cfg: dict,
    is_ground: bool,
    is_top: bool,
    add_stairs: bool,
    stair_loc: str,
    rng: random.Random,
) -> dict:
    """Rooms directly against exterior walls, no corridor."""
    fh = cfg["floor_height"]
    elev = floor_idx * fh
    et = cfg["ext_wall_t"]
    pt = cfg["part_wall_t"]

    walls = []
    rooms = []
    stairs = []

    w_south = _make_wall([0, 0], [w, 0], et, "exterior")
    w_east = _make_wall([w, 0], [w, d], et, "exterior")
    w_north = _make_wall([w, d], [0, d], et, "exterior")
    w_west = _make_wall([0, d], [0, 0], et, "exterior")

    if cfg.get("has_windows"):
        _place_windows_on_wall(w_south, cfg)
        _place_windows_on_wall(w_east, cfg)
        _place_windows_on_wall(w_north, cfg)
        _place_windows_on_wall(w_west, cfg)

    if is_ground:
        _place_door_center(w_south, cfg, width=cfg["main_entrance_width"])

    walls.extend([w_south, w_east, w_north, w_west])

    effective_rooms = max(1, num_rooms)
    room_w = w / effective_rooms

    for i in range(effective_rooms):
        x0 = i * room_w
        x1 = (i + 1) * room_w if i < effective_rooms - 1 else w

        if i > 0:
            part = _make_wall([x0, 0], [x0, d], pt, "partition")
            _place_door_center(part, cfg)
            walls.append(part)

        label = f"Room {i+1}"
        if cfg.get("room_labels") and i < len(cfg["room_labels"]):
            label = cfg["room_labels"][i]

        rooms.append({
            "room_id": _next_room_id(),
            "label": label,
            "polygon": [[x0, 0], [x1, 0], [x1, d], [x0, d]],
            "area_m2": round((x1 - x0) * d, 1),
            "usage": "other",
        })

    return {
        "floor_index": floor_idx,
        "label": f"{floor_idx + 1}F",
        "elevation_m": round(elev, 2),
        "height_m": fh,
        "walls": walls,
        "rooms": rooms,
        "stairs": stairs,
        "columns": [],
        "elevators": [],
    }


# ── main generation ───────────────────────────────────────────────────────────

FLOOR_GEN = {
    "single": _generate_floor_rect_single,
    "double": _generate_floor_rect_double,
    "none": _generate_floor_no_corridor,
}


def generate_floorplan(
    building_width: float,
    building_depth: float,
    num_floors: int,
    rooms_per_floor: int,
    has_windows: bool = True,
    door_placement: str = "every_room",
    **kwargs,
) -> dict:
    _reset_counters()

    cfg = dict(DEFAULTS)
    cfg.update({
        "has_windows": has_windows,
        "door_placement": door_placement,
        "num_floors": num_floors,
    })
    for k, v in kwargs.items():
        if v is not None:
            cfg[k] = v

    seed = cfg.get("seed", 0)
    rng = random.Random(seed if seed else None)

    corridor_type = cfg.get("corridor_type", "single")
    stair_loc = cfg.get("stair_location", "end")
    add_stairs = cfg.get("add_stairs", True)
    building_shape = cfg.get("building_shape", "rectangle")

    gen_fn = FLOOR_GEN.get(corridor_type, _generate_floor_rect_single)

    floors = []
    for fi in range(num_floors):
        is_ground = fi == 0
        is_top = fi == num_floors - 1
        floor = gen_fn(
            fi, building_width, building_depth, rooms_per_floor, cfg,
            is_ground, is_top, add_stairs, stair_loc, rng,
        )
        floors.append(floor)

    name_parts = []
    if cfg.get("building_name"):
        name_parts.append(cfg["building_name"])
    else:
        name_parts.append(f"Generated_{building_shape}")
        name_parts.append(f"{int(building_width)}x{int(building_depth)}")
        name_parts.append(f"{num_floors}F")

    return {
        "version": "1.0",
        "building_name": "_".join(name_parts),
        "source": "parameter_generator",
        "source_file": "",
        "scale": {"unit": "meter", "pixels_per_meter": 0, "origin": [0.0, 0.0]},
        "floors": floors,
        "bridges": [],
        "metadata": {
            "author": "floorplan_generator.py",
            "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "parameters": {
                "building_width": building_width,
                "building_depth": building_depth,
                "num_floors": num_floors,
                "rooms_per_floor": rooms_per_floor,
                "has_windows": has_windows,
                "door_placement": door_placement,
                "corridor_type": corridor_type,
                "building_shape": building_shape,
                "stair_location": stair_loc,
                "seed": seed,
            },
        },
    }


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate floorplan.json from parameters (no floor-plan drawing needed)")
    parser.add_argument("--width", type=float, required=True, help="Building width in meters")
    parser.add_argument("--depth", type=float, required=True, help="Building depth in meters")
    parser.add_argument("--floors", type=int, required=True, help="Number of floors")
    parser.add_argument("--rooms", type=int, required=True, help="Rooms per floor")
    parser.add_argument("--windows", action="store_true", help="Add windows to exterior walls")
    parser.add_argument("--doors", type=str, default="every_room",
                        choices=["every_room", "corridor_only", "manual"],
                        help="Door placement strategy")
    parser.add_argument("--corridor", type=str, default="single",
                        choices=["none", "single", "double", "L", "loop"])
    parser.add_argument("--shape", type=str, default="rectangle",
                        choices=["rectangle", "L", "T", "U"])
    parser.add_argument("--stairs", type=str, default="end",
                        choices=["end", "center", "both_ends"])
    parser.add_argument("--floor-height", type=float, default=3.5)
    parser.add_argument("--corridor-width", type=float, default=3.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--name", type=str, default="")
    parser.add_argument("--labels", type=str, nargs="*", default=[],
                        help="Room label overrides")
    parser.add_argument("--output", "-o", type=str, required=True, help="Output JSON path")

    args = parser.parse_args()

    data = generate_floorplan(
        building_width=args.width,
        building_depth=args.depth,
        num_floors=args.floors,
        rooms_per_floor=args.rooms,
        has_windows=args.windows,
        door_placement=args.doors,
        corridor_type=args.corridor,
        building_shape=args.shape,
        stair_location=args.stairs,
        floor_height=args.floor_height,
        corridor_width=args.corridor_width,
        seed=args.seed,
        building_name=args.name or None,
        room_labels=args.labels or None,
    )

    out_path = args.output
    if not os.path.isabs(out_path):
        out_path = os.path.join(os.getcwd(), out_path)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    total_walls = sum(len(fl["walls"]) for fl in data["floors"])
    total_rooms = sum(len(fl["rooms"]) for fl in data["floors"])
    total_openings = sum(
        len(op) for fl in data["floors"] for w in fl["walls"] for op in [w["openings"]]
    )

    print(f"\n=== floorplan_generator ===")
    print(f"  Building : {data['building_name']}")
    print(f"  Size     : {args.width}m x {args.depth}m, {args.floors}F")
    print(f"  Corridor : {args.corridor}")
    print(f"  Walls    : {total_walls}")
    print(f"  Rooms    : {total_rooms}")
    print(f"  Openings : {total_openings}")
    print(f"  Output   : {out_path}")
    print(f"===========================\n")


if __name__ == "__main__":
    main()
