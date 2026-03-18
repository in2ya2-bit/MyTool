"""
OSM Water Mask Builder
Fetches water bodies from OpenStreetMap and produces:
  - Boolean raster mask (True = water) for heightmap sea-level flattening
  - Structured JSON with polygon / linestring data for UE5 Water Plugin actors
"""

import math
import time
import json
import logging
import requests
import numpy as np
from PIL import Image, ImageDraw
from typing import Dict, List, Optional, Tuple

log = logging.getLogger(__name__)

OVERPASS_URL = "https://overpass-api.de/api/interpreter"


# ─── Overpass Query ───────────────────────────────────────────────────────────

def _build_query(center_lat: float, center_lon: float, radius_km: float) -> str:
    r = int(radius_km * 1000)
    return f"""
[out:json][timeout:90];
(
  way["natural"="water"](around:{r},{center_lat:.6f},{center_lon:.6f});
  way["natural"="coastline"](around:{r},{center_lat:.6f},{center_lon:.6f});
  way["landuse"="reservoir"](around:{r},{center_lat:.6f},{center_lon:.6f});
  way["waterway"~"^(river|canal|stream)$"](around:{r},{center_lat:.6f},{center_lon:.6f});
  relation["natural"="water"](around:{r},{center_lat:.6f},{center_lon:.6f});
);
out body;
>;
out skel qt;
"""


def _fetch_osm(center_lat: float, center_lon: float, radius_km: float) -> dict:
    query = _build_query(center_lat, center_lon, radius_km)
    log.info(f"Water Overpass query: ({center_lat:.4f},{center_lon:.4f}) r={radius_km}km")
    for attempt in range(3):
        try:
            resp = requests.post(OVERPASS_URL, data={"data": query}, timeout=90)
            resp.raise_for_status()
            data = resp.json()
            log.info(f"  Water elements received: {len(data.get('elements', []))}")
            return data
        except Exception as e:
            if attempt == 2:
                log.warning(f"Water OSM fetch failed: {e}")
                return {"elements": []}
            log.warning(f"  Attempt {attempt+1} failed, retrying… ({e})")
            time.sleep(3 * (attempt + 1))
    return {"elements": []}


# ─── Coordinate helpers ───────────────────────────────────────────────────────

def _latlon_to_px(
    lat: float, lon: float,
    lat_min: float, lat_max: float,
    lon_min: float, lon_max: float,
    h: int, w: int
) -> Tuple[int, int]:
    x = int((lon - lon_min) / max(lon_max - lon_min, 1e-9) * (w - 1))
    y = int((lat_max - lat) / max(lat_max - lat_min, 1e-9) * (h - 1))
    return max(0, min(w - 1, x)), max(0, min(h - 1, y))


def _latlon_to_ue5(
    lat: float, lon: float,
    origin_lat: float, origin_lon: float
) -> Tuple[float, float]:
    m_per_lat = 111320.0
    m_per_lon = 111320.0 * math.cos(math.radians(origin_lat))
    dx_m =  (lon - origin_lon) * m_per_lon
    dy_m = -(lat - origin_lat) * m_per_lat
    return round(dx_m * 100.0, 1), round(dy_m * 100.0, 1)


# ─── Main Build Function ──────────────────────────────────────────────────────

def build_water_mask(
    center_lat: float,
    center_lon: float,
    radius_km: float,
    grid_h: int,
    grid_w: int,
    output_json_path: Optional[str] = None
) -> Tuple[np.ndarray, dict]:
    """
    Fetch OSM water data, rasterize to a boolean mask, and produce UE5-ready JSON.

    Returns:
        mask       : np.ndarray bool  (True = water pixel), shape (grid_h, grid_w)
        water_json : dict { has_ocean, lakes:[{id, points_ue5}], rivers:[{...}] }
    """
    raw      = _fetch_osm(center_lat, center_lon, radius_km)
    elements = raw.get("elements", [])

    node_map: Dict[int, Tuple[float, float]] = {
        e["id"]: (e["lat"], e["lon"])
        for e in elements if e["type"] == "node"
    }

    # Bounding box
    half_lat = radius_km / 111.0
    half_lon = radius_km / (111.0 * math.cos(math.radians(center_lat)))
    lat_min = center_lat - half_lat;  lat_max = center_lat + half_lat
    lon_min = center_lon - half_lon;  lon_max = center_lon + half_lon

    img  = Image.new("L", (grid_w, grid_h), 0)
    draw = ImageDraw.Draw(img)

    has_ocean = False
    lakes:  List[dict] = []
    rivers: List[dict] = []

    for e in elements:
        if e["type"] != "way":
            continue

        tags  = e.get("tags", {})
        nodes = e.get("nodes", [])

        pixels: List[Tuple[int, int]] = []
        ue5pts: List[List[float]]     = []

        for nid in nodes:
            if nid not in node_map:
                continue
            lat, lon = node_map[nid]
            px = _latlon_to_px(lat, lon, lat_min, lat_max, lon_min, lon_max, grid_h, grid_w)
            pixels.append(px)
            ux, uy = _latlon_to_ue5(lat, lon, center_lat, center_lon)
            ue5pts.append([ux, uy])

        if len(pixels) < 2:
            continue

        is_closed = len(nodes) >= 3 and nodes[0] == nodes[-1]
        nat      = tags.get("natural", "")
        waterway = tags.get("waterway", "")
        landuse  = tags.get("landuse", "")

        if nat == "coastline":
            # Draw coastline as thick barrier for flood-fill sea detection
            has_ocean = True
            draw.line(pixels, fill=255, width=4)

        elif (nat == "water" or landuse == "reservoir") and is_closed and len(pixels) >= 3:
            draw.polygon(pixels, fill=255)
            lakes.append({
                "id":         e["id"],
                "points_ue5": ue5pts,
                "type":       nat or landuse,
            })

        elif waterway in ("river", "canal", "stream") and len(pixels) >= 2:
            try:
                raw_w  = tags.get("width", "5" if waterway == "river" else "3")
                width_m = float(str(raw_w).split()[0])
            except (ValueError, IndexError):
                width_m = 4.0

            m_per_px = (radius_km * 2000.0) / grid_w
            px_w     = max(2, int(width_m / max(m_per_px, 0.001)))
            draw.line(pixels, fill=255, width=min(px_w, 20))
            rivers.append({
                "id":         e["id"],
                "waterway":   waterway,
                "width_m":    width_m,
                "points_ue5": ue5pts,
            })

    mask = np.array(img) > 0

    # ── Flood-fill from corners to capture all connected sea pixels ───────────
    # The coastline drawn above acts as a barrier.  Any pixel reachable from
    # the 4 image corners without crossing the barrier = open sea.
    if has_ocean:
        from scipy.ndimage import label as _label
        barrier = mask.copy()  # coastline pixels = True (barrier)

        # Pad image so the 4 corners are all connected to a single "outside" region
        padded   = np.pad(barrier.astype(np.uint8), 1, mode="constant", constant_values=0)
        open_sea = ~padded.astype(bool)                   # non-barrier pixels
        labeled, _ = _label(open_sea)
        outer_label = labeled[0, 0]                        # corner is always outer sea

        if outer_label > 0:
            sea_region = (labeled == outer_label)[1:-1, 1:-1]  # trim padding
            sea_px = int(sea_region.sum())
            mask = mask | sea_region
            log.info(f"  Coastline flood-fill: {sea_px} additional sea pixels "
                     f"({100 * sea_region.mean():.1f}% of grid)")

    water_json: dict = {
        "has_ocean": has_ocean,
        "lakes":     lakes,
        "rivers":    rivers,
    }

    log.info(
        f"Water mask: ocean={has_ocean}  lakes={len(lakes)}  rivers={len(rivers)}"
        f"  coverage={100 * mask.mean():.1f}%"
    )

    if output_json_path:
        with open(output_json_path, "w", encoding="utf-8") as f:
            json.dump(water_json, f, indent=2, ensure_ascii=False)
        log.info(f"Water JSON saved: {output_json_path}")

    return mask, water_json
