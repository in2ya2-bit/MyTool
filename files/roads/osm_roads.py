"""
OSM Road Fetcher
Queries OpenStreetMap via Overpass API for road network.

Highway type classification:
  major : motorway, trunk, primary, secondary
  minor : tertiary, residential, unclassified, service
  path  : footway, cycleway, path, pedestrian, steps
"""

import math
import time
import json
import logging
import requests
from typing import List, Dict, Optional

log = logging.getLogger(__name__)

OVERPASS_URL = "https://overpass-api.de/api/interpreter"

HIGHWAY_MAJOR = {"motorway", "trunk", "primary", "secondary",
                 "motorway_link", "trunk_link", "primary_link", "secondary_link"}
HIGHWAY_MINOR = {"tertiary", "tertiary_link", "residential", "unclassified",
                 "service", "living_street", "road"}
HIGHWAY_PATH  = {"footway", "cycleway", "path", "pedestrian", "steps",
                 "track", "bridleway"}


def classify_highway(highway_val: str) -> str:
    v = highway_val.lower()
    if v in HIGHWAY_MAJOR: return "major"
    if v in HIGHWAY_MINOR: return "minor"
    if v in HIGHWAY_PATH:  return "path"
    return "minor"  # fallback


DEFAULT_WIDTH_M = {"major": 12.0, "minor": 6.0, "path": 2.0}

def parse_road_width(tags: dict, road_type: str) -> float:
    """Extract road width in meters from OSM tags, with type-based defaults."""
    if "width" in tags:
        try:
            return float(str(tags["width"]).replace("m", "").strip().split()[0])
        except (ValueError, IndexError):
            pass

    if "lanes" in tags:
        try:
            lanes = int(tags["lanes"])
            return lanes * 3.5
        except (ValueError, TypeError):
            pass

    return DEFAULT_WIDTH_M.get(road_type, 6.0)


def build_overpass_query(center_lat: float, center_lon: float, radius_km: float) -> str:
    radius_m = int(radius_km * 1000)
    return f"""
[out:json][timeout:90];
(
  way["highway"]
    (around:{radius_m},{center_lat:.6f},{center_lon:.6f});
);
out body;
>;
out skel qt;
"""


def latlon_to_ue5_xy(lat, lon, origin_lat, origin_lon, ue_unit_per_meter=100.0):
    m_per_lat = 111320.0
    m_per_lon = 111320.0 * math.cos(math.radians(origin_lat))
    dx_m =  (lon - origin_lon) * m_per_lon
    dy_m = -(lat - origin_lat) * m_per_lat
    return dx_m * ue_unit_per_meter, dy_m * ue_unit_per_meter


def fetch_osm_roads_raw(center_lat, center_lon, radius_km) -> dict:
    query = build_overpass_query(center_lat, center_lon, radius_km)
    log.info(f"Overpass road query: center=({center_lat:.4f},{center_lon:.4f}), radius={radius_km}km")
    for attempt in range(3):
        try:
            resp = requests.post(OVERPASS_URL, data={"data": query}, timeout=90)
            resp.raise_for_status()
            data = resp.json()
            log.info(f"  Received {len(data.get('elements', []))} elements")
            return data
        except Exception as e:
            if attempt == 2:
                log.warning(f"Road fetch failed after 3 attempts: {e}")
                return {"elements": []}
            log.warning(f"  Attempt {attempt+1} failed: {e}, retrying...")
            time.sleep(3 * (attempt + 1))
    return {"elements": []}


def parse_roads(osm_data: dict, origin_lat: float, origin_lon: float) -> List[Dict]:
    elements = osm_data.get("elements", [])

    node_map = {
        e["id"]: (e["lat"], e["lon"])
        for e in elements
        if e["type"] == "node"
    }

    roads = []
    for e in elements:
        if e["type"] != "way":
            continue
        tags = e.get("tags", {})
        highway_val = tags.get("highway", "")
        if not highway_val:
            continue

        node_refs = e.get("nodes", [])
        points_ue5 = []
        for nid in node_refs:
            if nid in node_map:
                lat, lon = node_map[nid]
                x, y = latlon_to_ue5_xy(lat, lon, origin_lat, origin_lon)
                points_ue5.append([round(x, 1), round(y, 1)])

        if len(points_ue5) < 2:
            continue

        rtype = classify_highway(highway_val)
        roads.append({
            "id":         e["id"],
            "type":       rtype,
            "highway":    highway_val,
            "name":       tags.get("name", tags.get("name:en", "")),
            "width_m":    round(parse_road_width(tags, rtype), 1),
            "points_ue5": points_ue5,
        })

    log.info(f"Parsed {len(roads)} roads")
    return roads


def save_roads_json(roads: List[Dict], path: str):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(roads, f, indent=2, ensure_ascii=False)
    log.info(f"Roads JSON saved: {path}  ({len(roads)} roads)")


def fetch_and_parse_roads(
    center_lat:       float,
    center_lon:       float,
    radius_km:        float,
    output_json_path: Optional[str] = None
) -> List[Dict]:
    try:
        raw   = fetch_osm_roads_raw(center_lat, center_lon, radius_km)
        roads = parse_roads(raw, center_lat, center_lon)
        if output_json_path and roads:
            save_roads_json(roads, output_json_path)
        return roads
    except Exception as e:
        log.warning(f"Road fetch skipped: {e}")
        return []
