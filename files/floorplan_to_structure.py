"""
F-5: floorplan.json → structure_layout.json Converter

Converts the floorplan pipeline output into the StructureGenerator's
structure_layout.json format for editor integration.

Usage:
  python floorplan_to_structure.py --input floorplan.json --output structure_layout.json
  python floorplan_to_structure.py --input floorplan.json --output structure_layout.json \
      --structure-id bldg_osm_123 --structure-type factory
"""

import argparse
import json
import math
import os
from datetime import datetime, timezone
from typing import Optional

METER_TO_CM = 100.0

USAGE_TO_ZONE_TYPE = {
    "lobby": "lobby",
    "corridor": "corridor",
    "office": "admin",
    "classroom": "classroom",
    "restroom": "utility",
    "utility": "utility",
    "stairwell": "stairs",
    "production": "production",
    "storage": "storage",
    "loading": "loading_dock",
    "cafeteria": "cafeteria",
    "gymnasium": "gymnasium",
    "control": "control_room",
    "other": "other",
}


def _polygon_to_aabb_ue5(polygon: list) -> dict:
    """Convert a meter polygon to UE5 AABB (cm)."""
    xs = [p[0] for p in polygon]
    ys = [p[1] for p in polygon]
    return {
        "min": [round(min(xs) * METER_TO_CM, 1), round(min(ys) * METER_TO_CM, 1)],
        "max": [round(max(xs) * METER_TO_CM, 1), round(max(ys) * METER_TO_CM, 1)],
    }


def _point_to_ue5(pt: list) -> list:
    """Convert [x, y] meter to [x, y] cm."""
    return [round(pt[0] * METER_TO_CM, 1), round(pt[1] * METER_TO_CM, 1)]


def _point3d_to_ue5(pt: list) -> list:
    """Convert [x, y, z] meter to [x, y, z] cm."""
    return [round(p * METER_TO_CM, 1) for p in pt]


def _wall_facing(wall: dict) -> str:
    """Estimate wall facing direction from start/end."""
    sx, sy = wall["start"]
    ex, ey = wall["end"]
    dx, dy = ex - sx, ey - sy
    angle = math.degrees(math.atan2(dy, dx)) % 360

    if 315 <= angle or angle < 45:
        return "east"
    elif 45 <= angle < 135:
        return "north"
    elif 135 <= angle < 225:
        return "west"
    else:
        return "south"


def _door_position(opening: dict, wall: dict) -> str:
    """Determine door position label relative to wall direction."""
    return _wall_facing(wall)


def _classify_door_type(opening: dict) -> str:
    """Classify door type based on width."""
    w = opening.get("width_m", 1.0)
    if w >= 1.8:
        return "double"
    return "single"


def convert_floor(floor: dict, all_walls: list) -> dict:
    """Convert a single floorplan floor into structure_layout floor."""

    rooms_by_usage = {}
    for room in floor.get("rooms", []):
        usage = room.get("usage", "other")
        zone_type = USAGE_TO_ZONE_TYPE.get(usage, usage)
        if zone_type not in rooms_by_usage:
            rooms_by_usage[zone_type] = []
        rooms_by_usage[zone_type].append(room)

    zones = []
    for zone_type, rooms in rooms_by_usage.items():
        all_polys = [r["polygon"] for r in rooms if r.get("polygon")]
        if not all_polys:
            continue

        all_xs = [p[0] for poly in all_polys for p in poly]
        all_ys = [p[1] for poly in all_polys for p in poly]
        zone_aabb = {
            "min": [round(min(all_xs) * METER_TO_CM, 1), round(min(all_ys) * METER_TO_CM, 1)],
            "max": [round(max(all_xs) * METER_TO_CM, 1), round(max(all_ys) * METER_TO_CM, 1)],
        }

        converted_rooms = []
        for room in rooms:
            poly = room.get("polygon", [])
            aabb = _polygon_to_aabb_ue5(poly) if poly else {"min": [0, 0], "max": [0, 0]}

            doors = []
            room_walls = _find_walls_for_room(room, all_walls)
            for wall in room_walls:
                for op in wall.get("openings", []):
                    if op["type"] == "door":
                        doors.append({
                            "wall_id": wall["wall_id"],
                            "position": _door_position(op, wall),
                            "type": _classify_door_type(op),
                        })

            converted_rooms.append({
                "room_id": room["room_id"],
                "aabb_ue5": aabb,
                "height_m": floor.get("height_m", 3.2),
                "is_void": room.get("area_m2", 0) > 100,
                "doors": doors,
            })

        zone_id = f"zone_{floor['floor_index']}_{zone_type}"
        zones.append({
            "zone_id": zone_id,
            "zone_type": zone_type,
            "aabb_ue5": zone_aabb,
            "rooms": converted_rooms,
        })

    corridors = []
    for room in floor.get("rooms", []):
        if room.get("usage") == "corridor":
            poly = room.get("polygon", [])
            if len(poly) >= 2:
                path = [_point_to_ue5(p) for p in poly]
                xs = [p[0] for p in poly]
                ys = [p[1] for p in poly]
                w = max(xs) - min(xs)
                h = max(ys) - min(ys)
                width = round(min(w, h), 2)
                corridors.append({
                    "corridor_id": f"corr_{floor['floor_index']}_{room['room_id']}",
                    "path_ue5": path,
                    "width_m": width,
                    "pattern": "single_loaded",
                })

    stairs = []
    for stair in floor.get("stairs", []):
        loc = stair.get("position", stair.get("center", [0, 0]))
        size = stair.get("size_m", [3, 5])
        stairs.append({
            "stair_id": stair.get("stair_id", f"stair_{floor['floor_index']}"),
            "location_ue5": _point_to_ue5(loc),
            "size_m": size,
            "type": stair.get("type", "straight"),
            "connects_to_floor": [
                floor["floor_index"],
                floor["floor_index"] + 1,
            ],
        })

    return {
        "floor_index": floor["floor_index"],
        "elevation_m": floor.get("elevation_m", 0),
        "zones": zones,
        "corridors": corridors,
        "stairs": stairs,
    }


def _find_walls_for_room(room: dict, walls: list) -> list:
    """Find walls that border a room (simple proximity check)."""
    poly = room.get("polygon", [])
    if not poly:
        return []

    result = []
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)

    for wall in walls:
        wx = (wall["start"][0] + wall["end"][0]) / 2
        wy = (wall["start"][1] + wall["end"][1]) / 2
        xs = [p[0] for p in poly]
        ys = [p[1] for p in poly]
        margin = 0.5
        if (min(xs) - margin <= wx <= max(xs) + margin and
                min(ys) - margin <= wy <= max(ys) + margin):
            result.append(wall)

    return result


def convert_entrances(data: dict) -> list:
    """Detect building entrances from exterior wall doors."""
    entrances = []
    ec = 0
    for floor in data.get("floors", []):
        if floor["floor_index"] != 0:
            continue
        for wall in floor.get("walls", []):
            if wall.get("type") != "exterior":
                continue
            for op in wall.get("openings", []):
                if op["type"] == "door":
                    ec += 1
                    sx, sy = wall["start"]
                    ex, ey = wall["end"]
                    dx, dy = ex - sx, ey - sy
                    wlen = math.sqrt(dx * dx + dy * dy)
                    if wlen < 0.01:
                        continue
                    t = (op["offset_m"] + op["width_m"] / 2) / wlen
                    door_x = sx + dx * t
                    door_y = sy + dy * t
                    angle = math.degrees(math.atan2(dy, dx)) % 360

                    facing_deg = (angle + 90) % 360

                    entrances.append({
                        "entrance_id": f"ent_{ec:02d}",
                        "location_ue5": [
                            round(door_x * METER_TO_CM, 1),
                            round(door_y * METER_TO_CM, 1),
                            round(floor.get("elevation_m", 0) * METER_TO_CM, 1),
                        ],
                        "facing_deg": round(facing_deg, 0),
                        "type": _classify_door_type(op),
                        "connected_room": "",
                    })

    return entrances


def convert_cover_hints(data: dict) -> list:
    """Convert columns to cover hints."""
    hints = []
    for floor in data.get("floors", []):
        elev = floor.get("elevation_m", 0)
        height = floor.get("height_m", 3.2)
        for col in floor.get("columns", []):
            center = col.get("center", [0, 0])
            hints.append({
                "location_ue5": [
                    round(center[0] * METER_TO_CM, 1),
                    round(center[1] * METER_TO_CM, 1),
                    round((elev + height / 2) * METER_TO_CM, 1),
                ],
                "type": "cover_pillar",
                "sightline_length_m": 0,
            })

    return hints


def compute_zone_ratios(data: dict) -> dict:
    """Compute zone type ratios from all rooms."""
    area_by_type = {}
    total_area = 0
    for floor in data.get("floors", []):
        for room in floor.get("rooms", []):
            usage = room.get("usage", "other")
            zone_type = USAGE_TO_ZONE_TYPE.get(usage, usage)
            a = room.get("area_m2", 0)
            area_by_type[zone_type] = area_by_type.get(zone_type, 0) + a
            total_area += a

    if total_area < 0.01:
        return {}

    return {k: round(v / total_area, 2) for k, v in sorted(area_by_type.items())}


def convert_floorplan(data: dict, structure_id: str = "",
                       structure_type: str = "") -> dict:
    """Convert full floorplan.json to structure_layout.json."""

    name = data.get("building_name", "unknown")
    s_id = structure_id or f"bldg_{name}"
    s_type = structure_type or _infer_structure_type(data)

    all_walls = []
    for floor in data.get("floors", []):
        all_walls.extend(floor.get("walls", []))

    footprint_ue5 = []
    if data.get("floors"):
        first_floor = data["floors"][0]
        exterior_pts = set()
        for wall in first_floor.get("walls", []):
            if wall.get("type") == "exterior":
                s = tuple(wall["start"])
                e = tuple(wall["end"])
                exterior_pts.add(s)
                exterior_pts.add(e)
        if exterior_pts:
            pts = list(exterior_pts)
            cx = sum(p[0] for p in pts) / len(pts)
            cy = sum(p[1] for p in pts) / len(pts)
            pts.sort(key=lambda p: math.atan2(p[1] - cy, p[0] - cx))
            footprint_ue5 = [_point_to_ue5(list(p)) for p in pts]

    floor_count = len(data.get("floors", []))
    floor_height = data["floors"][0]["height_m"] if data.get("floors") else 3.2

    converted_floors = []
    for floor in data.get("floors", []):
        cf = convert_floor(floor, all_walls)
        converted_floors.append(cf)

    result = {
        "version": "1.0",
        "structure_id": s_id,
        "structure_type": s_type,
        "footprint_ue5": footprint_ue5,
        "floor_count": floor_count,
        "floor_height_m": floor_height,
        "generation_seed": 0,
        "zone_ratios": compute_zone_ratios(data),
        "floors": converted_floors,
        "entrances": convert_entrances(data),
        "cover_hints": convert_cover_hints(data),
        "edit_operations": [],
        "metadata": {
            "source": "floorplan_to_structure.py",
            "source_floorplan": data.get("source_file", ""),
            "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
    }

    return result


def _infer_structure_type(data: dict) -> str:
    """Infer structure type from building name or room composition."""
    name = data.get("building_name", "").lower()
    for keyword, stype in [
        ("school", "school"), ("hospital", "hospital"), ("factory", "factory"),
        ("office", "office"), ("warehouse", "warehouse"), ("church", "church"),
        ("station", "station"), ("apartment", "apartment"),
    ]:
        if keyword in name:
            return stype
    return "generic"


def main():
    parser = argparse.ArgumentParser(
        description="Convert floorplan.json → structure_layout.json"
    )
    parser.add_argument("--input", "-i", required=True, help="Input floorplan.json")
    parser.add_argument("--output", "-o", required=True, help="Output structure_layout.json")
    parser.add_argument("--structure-id", type=str, default="")
    parser.add_argument("--structure-type", type=str, default="")

    args = parser.parse_args()

    print(f"\n=== Floorplan → StructureLayout Converter ===")
    print(f"  Input : {args.input}")
    print(f"  Output: {args.output}")

    with open(args.input, encoding="utf-8") as f:
        data = json.load(f)

    result = convert_floorplan(data, args.structure_id, args.structure_type)

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)

    n_zones = sum(len(fl.get("zones", [])) for fl in result["floors"])
    n_rooms = sum(
        len(z.get("rooms", []))
        for fl in result["floors"] for z in fl.get("zones", [])
    )
    n_ent = len(result["entrances"])
    n_cover = len(result["cover_hints"])

    print(f"\n  Structure ID   : {result['structure_id']}")
    print(f"  Structure Type : {result['structure_type']}")
    print(f"  Floors         : {result['floor_count']}")
    print(f"  Zones          : {n_zones}")
    print(f"  Rooms          : {n_rooms}")
    print(f"  Entrances      : {n_ent}")
    print(f"  Cover Hints    : {n_cover}")
    print(f"  Zone Ratios    : {result['zone_ratios']}")
    print(f"\n  Saved: {args.output}")
    print(f"=== Done ===\n")


if __name__ == "__main__":
    main()
