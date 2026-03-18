"""
OSM Building Fetcher
Queries OpenStreetMap via Overpass API for building footprints.

Returns structured building data ready for:
  - Blender extrude pipeline
  - UE5 procedural placement
"""

import math
import time
import json
import logging
import requests
import numpy as np
from typing import List, Dict, Optional

log = logging.getLogger(__name__)

OVERPASS_URL = "https://overpass-api.de/api/interpreter"


# ─── Overpass Query Builder ───────────────────────────────────────────────────

def build_overpass_query(
    center_lat: float,
    center_lon: float,
    radius_km:  float
) -> str:
    """
    Build Overpass QL query for buildings in a circular area.
    Fetches: footprint polygons + height/levels/type metadata.
    """
    radius_m = int(radius_km * 1000)
    return f"""
[out:json][timeout:60];
(
  way["building"]
    (around:{radius_m},{center_lat:.6f},{center_lon:.6f});
  relation["building"]
    (around:{radius_m},{center_lat:.6f},{center_lon:.6f});
);
out body;
>;
out skel qt;
"""


# ─── Fetch Raw OSM Data ───────────────────────────────────────────────────────

def fetch_osm_buildings_raw(
    center_lat: float,
    center_lon: float,
    radius_km:  float
) -> dict:
    """Query Overpass API and return raw JSON response."""
    query = build_overpass_query(center_lat, center_lon, radius_km)

    log.info(f"Overpass query: center=({center_lat:.4f},{center_lon:.4f}), radius={radius_km}km")

    for attempt in range(3):
        try:
            resp = requests.post(
                OVERPASS_URL,
                data={"data": query},
                timeout=90
            )
            resp.raise_for_status()
            data = resp.json()
            log.info(f"  Received {len(data.get('elements', []))} elements")
            return data
        except Exception as e:
            if attempt == 2:
                raise RuntimeError(f"Overpass API failed after 3 attempts: {e}")
            log.warning(f"  Attempt {attempt+1} failed: {e}, retrying...")
            time.sleep(3 * (attempt + 1))


# ─── Parse OSM Elements ───────────────────────────────────────────────────────

def parse_height(tags: dict, default_m: float = 10.0) -> float:
    """Extract height in meters from OSM tags."""
    # Prefer explicit height tag
    if "height" in tags:
        val = tags["height"].replace(" m", "").replace("m", "").strip()
        try:
            return float(val)
        except ValueError:
            pass

    # Fallback: building:levels × 3m per floor
    if "building:levels" in tags:
        try:
            return float(tags["building:levels"]) * 3.0
        except ValueError:
            pass

    return default_m


def classify_building(tags: dict) -> str:
    """Map OSM building tag to UE5 mesh category."""
    from config import BUILDING_TYPE_MAP

    building_val = tags.get("building", "yes").lower()
    amenity_val  = tags.get("amenity", "").lower()
    landuse_val  = tags.get("landuse", "").lower()

    # Check specific type first
    if building_val in BUILDING_TYPE_MAP:
        return BUILDING_TYPE_MAP[building_val]

    # Amenity-based fallback
    if amenity_val in {"school", "university", "college"}:
        return BUILDING_TYPE_MAP["school"]
    if amenity_val in {"hospital", "clinic"}:
        return BUILDING_TYPE_MAP["hospital"]
    if amenity_val in {"place_of_worship", "church", "mosque"}:
        return BUILDING_TYPE_MAP["church"]

    return BUILDING_TYPE_MAP["yes"]  # Generic


def compute_footprint_area(polygon_latlon: List[tuple]) -> float:
    """Approximate polygon area in m² using Shoelace + lat/lon scaling."""
    if len(polygon_latlon) < 3:
        return 0.0

    # Convert to approximate XY meters (relative to first point)
    lat0, lon0 = polygon_latlon[0]
    m_per_lat = 111320.0
    m_per_lon = 111320.0 * math.cos(math.radians(lat0))

    pts = [
        ((lat - lat0) * m_per_lat, (lon - lon0) * m_per_lon)
        for lat, lon in polygon_latlon
    ]

    # Shoelace formula
    n   = len(pts)
    area = 0.0
    for i in range(n):
        j = (i + 1) % n
        area += pts[i][0] * pts[j][1]
        area -= pts[j][0] * pts[i][1]
    return abs(area) / 2.0


# ─── Coordinate Conversion ───────────────────────────────────────────────────

def latlon_to_ue5_xy(
    lat: float,
    lon: float,
    origin_lat: float,
    origin_lon: float,
    ue_unit_per_meter: float = 100.0
) -> tuple:
    """
    Convert lat/lon to UE5 local XY (cm).
    X = East, Y = South (UE5 coordinate convention).
    """
    m_per_lat = 111320.0
    m_per_lon = 111320.0 * math.cos(math.radians(origin_lat))

    dx_m =  (lon - origin_lon) * m_per_lon
    dy_m = -(lat - origin_lat) * m_per_lat  # UE5 Y+ = South

    return dx_m * ue_unit_per_meter, dy_m * ue_unit_per_meter


# ─── Main Parse Pipeline ─────────────────────────────────────────────────────

def parse_buildings(
    osm_data:         dict,
    origin_lat:       float,
    origin_lon:       float,
    min_area_m2:      float = 20.0,
    default_height_m: float = 10.0
) -> List[Dict]:
    """
    Convert raw Overpass JSON into structured building list.
    Each building dict:
      {
        id, type, height_m, area_m2,
        footprint_latlon: [(lat,lon), ...],
        footprint_ue5:    [(x,y), ...],  # UE5 cm
        centroid_ue5:     (x, y),
        tags: {...}
      }
    """
    elements = osm_data.get("elements", [])

    # Build node ID → (lat, lon) lookup
    node_map = {
        e["id"]: (e["lat"], e["lon"])
        for e in elements
        if e["type"] == "node"
    }

    buildings = []

    for e in elements:
        if e["type"] != "way":
            continue
        tags = e.get("tags", {})
        if "building" not in tags:
            continue

        # Reconstruct polygon from node refs
        node_refs = e.get("nodes", [])
        footprint_latlon = []
        for nid in node_refs:
            if nid in node_map:
                footprint_latlon.append(node_map[nid])

        if len(footprint_latlon) < 3:
            continue

        area = compute_footprint_area(footprint_latlon)
        if area < min_area_m2:
            continue

        height_m     = parse_height(tags, default_height_m)
        building_type = classify_building(tags)

        # Convert footprint to UE5 coords
        footprint_ue5 = [
            latlon_to_ue5_xy(lat, lon, origin_lat, origin_lon)
            for lat, lon in footprint_latlon
        ]

        # Centroid
        cx = sum(p[0] for p in footprint_ue5) / len(footprint_ue5)
        cy = sum(p[1] for p in footprint_ue5) / len(footprint_ue5)

        buildings.append({
            "id":               e["id"],
            "type":             building_type,
            "height_m":         round(height_m, 1),
            "height_ue5":       round(height_m * 100.0, 1),  # cm
            "area_m2":          round(area, 1),
            "footprint_latlon": footprint_latlon,
            "footprint_ue5":    footprint_ue5,
            "centroid_ue5":     (round(cx, 1), round(cy, 1)),
            "tags":             tags,
        })

    log.info(f"Parsed {len(buildings)} valid buildings (min area {min_area_m2}m²)")
    return buildings


# ─── Save / Load ─────────────────────────────────────────────────────────────

def save_buildings_json(buildings: List[Dict], path: str):
    """Save parsed building list to JSON for later use."""
    serializable = []
    for b in buildings:
        bc = b.copy()
        # Convert tuples to lists for JSON
        bc["footprint_latlon"] = [list(p) for p in b["footprint_latlon"]]
        bc["footprint_ue5"]    = [list(p) for p in b["footprint_ue5"]]
        bc["centroid_ue5"]     = list(b["centroid_ue5"])
        serializable.append(bc)

    with open(path, "w", encoding="utf-8") as f:
        json.dump(serializable, f, indent=2, ensure_ascii=False)
    log.info(f"Buildings JSON saved: {path}  ({len(buildings)} buildings)")


def load_buildings_json(path: str) -> List[Dict]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    # Restore tuples
    for b in data:
        b["footprint_latlon"] = [tuple(p) for p in b["footprint_latlon"]]
        b["footprint_ue5"]    = [tuple(p) for p in b["footprint_ue5"]]
        b["centroid_ue5"]     = tuple(b["centroid_ue5"])
    return data


# ─── Unified Fetch + Parse ────────────────────────────────────────────────────

def fetch_and_parse_buildings(
    center_lat:       float,
    center_lon:       float,
    radius_km:        float,
    output_json_path: Optional[str] = None,
    min_area_m2:      float = 20.0,
    default_height_m: float = 10.0
) -> List[Dict]:
    """
    Full pipeline: Overpass API → parsed building list → optional JSON save.
    """
    raw     = fetch_osm_buildings_raw(center_lat, center_lon, radius_km)
    bldgs   = parse_buildings(raw, center_lat, center_lon, min_area_m2, default_height_m)

    if output_json_path:
        save_buildings_json(bldgs, output_json_path)

    return bldgs
