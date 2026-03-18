"""
Elevation Fetcher
Retrieves elevation grid data from multiple sources.

Sources (priority order):
  1. open-elevation.com  — free, no key, global SRTM
  2. OpenTopography REST  — free account key, 30m SRTM
  3. Google Maps Elevation API  — paid, highest accuracy
"""

import math
import time
import logging
import os
import hashlib
import requests
import numpy as np
from typing import Tuple

log = logging.getLogger(__name__)

_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output", "elev_cache")
os.makedirs(_CACHE_DIR, exist_ok=True)


def _cache_key(center_lat, center_lon, radius_km, grid_size, source):
    s = f"{center_lat:.4f}_{center_lon:.4f}_{radius_km:.2f}_{grid_size}_{source}"
    return hashlib.md5(s.encode()).hexdigest()


def _load_cache(key):
    path = os.path.join(_CACHE_DIR, f"{key}.npy")
    if os.path.exists(path):
        log.info(f"Elevation cache hit — loading from {path}")
        return np.load(path)
    return None


def _save_cache(key, data):
    path = os.path.join(_CACHE_DIR, f"{key}.npy")
    np.save(path, data)
    log.info(f"Elevation cached → {path}")


# ─── Coordinate Grid Generation ──────────────────────────────────────────────

def build_latlon_grid(
    center_lat: float,
    center_lon: float,
    radius_km:  float,
    grid_size:  int
) -> Tuple[np.ndarray, np.ndarray]:
    """
    Returns (lat_grid, lon_grid) each shape (grid_size, grid_size).
    Covers a square region of ±radius_km around center.
    """
    # 1 degree latitude ≈ 111.32 km everywhere
    # 1 degree longitude ≈ 111.32 * cos(lat) km
    lat_delta = radius_km / 111.32
    lon_delta = radius_km / (111.32 * math.cos(math.radians(center_lat)))

    lats = np.linspace(center_lat - lat_delta, center_lat + lat_delta, grid_size)
    lons = np.linspace(center_lon - lon_delta, center_lon + lon_delta, grid_size)

    lon_grid, lat_grid = np.meshgrid(lons, lats)
    return lat_grid, lon_grid


# ─── Source 1: Open-Elevation (Free, SRTM 30m) ───────────────────────────────

def fetch_open_elevation(
    lat_grid: np.ndarray,
    lon_grid: np.ndarray,
    batch_size: int = 2000
) -> np.ndarray:
    """
    Query open-elevation.com for each grid point.
    Returns elevation array (meters) same shape as input grids.
    """
    rows, cols = lat_grid.shape
    flat_lats  = lat_grid.flatten()
    flat_lons  = lon_grid.flatten()
    elevations = np.zeros(len(flat_lats), dtype=np.float32)

    total_batches = math.ceil(len(flat_lats) / batch_size)
    log.info(f"Open-Elevation: fetching {len(flat_lats)} points in {total_batches} batches")

    for i in range(0, len(flat_lats), batch_size):
        batch_lats = flat_lats[i:i + batch_size]
        batch_lons = flat_lons[i:i + batch_size]

        locations = [
            {"latitude": float(la), "longitude": float(lo)}
            for la, lo in zip(batch_lats, batch_lons)
        ]

        for attempt in range(3):
            try:
                resp = requests.post(
                    "https://api.open-elevation.com/api/v1/lookup",
                    json={"locations": locations},
                    timeout=60
                )
                resp.raise_for_status()
                results = resp.json()["results"]
                for j, r in enumerate(results):
                    elevations[i + j] = r["elevation"]
                batch_num = i // batch_size + 1
                log.info(f"  Batch {batch_num}/{total_batches} done")
                time.sleep(0.05)
                break
            except Exception as e:
                if attempt == 2:
                    log.warning(f"  Batch {i//batch_size} failed after 3 attempts: {e}")
                else:
                    time.sleep(2 ** attempt)

    return elevations.reshape(rows, cols)


# ─── Source 2: OpenTopography SRTM REST ──────────────────────────────────────

def fetch_opentopography_srtm(
    center_lat: float,
    center_lon: float,
    radius_km:  float,
    grid_size:  int,
    api_key:    str = ""
) -> np.ndarray:
    """
    Uses OpenTopography GlobalDEM REST API (SRTMGL1 = 30m resolution).
    Returns a (grid_size × grid_size) elevation array.
    Requires free API key from opentopography.org.
    """
    lat_delta = radius_km / 111.32
    lon_delta = radius_km / (111.32 * math.cos(math.radians(center_lat)))

    south = center_lat - lat_delta
    north = center_lat + lat_delta
    west  = center_lon - lon_delta
    east  = center_lon + lon_delta

    params = {
        "demtype":    "SRTMGL1",
        "south":      south,
        "north":      north,
        "west":       west,
        "east":       east,
        "outputFormat": "AAIGrid",
        "API_Key":    api_key,
    }

    log.info(f"OpenTopography SRTM: bbox [{south:.4f},{west:.4f}] → [{north:.4f},{east:.4f}]")

    resp = requests.get(
        "https://portal.opentopography.org/API/globaldem",
        params=params,
        timeout=120
    )
    resp.raise_for_status()

    # Parse AAIGrid format
    lines = resp.text.strip().split("\n")
    header = {}
    data_start = 0
    for idx, line in enumerate(lines):
        parts = line.split()
        if parts[0].lower() in {"ncols","nrows","xllcorner","xllcenter",
                                 "yllcorner","yllcenter","cellsize","nodata_value"}:
            header[parts[0].lower()] = float(parts[1])
            data_start = idx + 1
        else:
            break

    raw_rows = []
    for line in lines[data_start:]:
        raw_rows.append([float(v) for v in line.split()])

    raw = np.array(raw_rows, dtype=np.float32)
    nodata = header.get("nodata_value", -9999)
    raw[raw == nodata] = 0.0

    # Resize to target grid_size using scipy
    from scipy.ndimage import zoom
    scale_r = grid_size / raw.shape[0]
    scale_c = grid_size / raw.shape[1]
    resized = zoom(raw, (scale_r, scale_c), order=1)

    return resized.astype(np.float32)


# ─── Source 3: Google Maps Elevation API ─────────────────────────────────────

def fetch_google_elevation(
    lat_grid:  np.ndarray,
    lon_grid:  np.ndarray,
    api_key:   str,
    batch_size: int = 512
) -> np.ndarray:
    """
    Google Maps Elevation API — most accurate, paid ($5 per 1000 requests).
    Returns elevation array (meters).
    """
    rows, cols = lat_grid.shape
    flat_lats  = lat_grid.flatten()
    flat_lons  = lon_grid.flatten()
    elevations = np.zeros(len(flat_lats), dtype=np.float32)

    total_batches = math.ceil(len(flat_lats) / batch_size)
    log.info(f"Google Elevation: fetching {len(flat_lats)} points in {total_batches} batches")

    for i in range(0, len(flat_lats), batch_size):
        batch_lats = flat_lats[i:i + batch_size]
        batch_lons = flat_lons[i:i + batch_size]

        locations = "|".join(f"{la:.6f},{lo:.6f}" for la, lo in zip(batch_lats, batch_lons))
        params = {"locations": locations, "key": api_key}

        resp = requests.get(
            "https://maps.googleapis.com/maps/api/elevation/json",
            params=params,
            timeout=30
        )
        resp.raise_for_status()
        data = resp.json()

        if data["status"] != "OK":
            raise RuntimeError(f"Google Elevation API error: {data['status']}")

        for j, result in enumerate(data["results"]):
            elevations[i + j] = result["elevation"]

        batch_num = i // batch_size + 1
        log.info(f"  Batch {batch_num}/{total_batches} done (resolution: {data['results'][0].get('resolution',0):.1f}m)")

    return elevations.reshape(rows, cols)


# ─── Unified Fetch Interface ──────────────────────────────────────────────────

def fetch_elevation(
    center_lat:  float,
    center_lon:  float,
    radius_km:   float,
    grid_size:   int,
    source:      str = "open_elevation",
    api_key:     str = ""
) -> np.ndarray:
    """
    Unified elevation fetch with disk cache.
    source: "open_elevation" | "opentopography" | "google"
    Returns float32 ndarray (grid_size × grid_size) in meters.
    """
    key = _cache_key(center_lat, center_lon, radius_km, grid_size, source)
    cached = _load_cache(key)
    if cached is not None:
        return cached

    lat_grid, lon_grid = build_latlon_grid(center_lat, center_lon, radius_km, grid_size)

    if source == "google":
        if not api_key:
            raise ValueError("Google Maps API key is required for source='google'")
        result = fetch_google_elevation(lat_grid, lon_grid, api_key)

    elif source == "opentopography":
        result = fetch_opentopography_srtm(
            center_lat, center_lon, radius_km, grid_size, api_key
        )

    else:  # open_elevation (default)
        result = fetch_open_elevation(lat_grid, lon_grid)

    _save_cache(key, result)
    return result
