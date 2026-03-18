"""
Heightmap Exporter
Converts float32 elevation array → UE5-ready heightmap assets.

Outputs:
  - .png   16-bit grayscale (UE5 Import Landscape → From File)
  - .r16   raw 16-bit little-endian (UE5 alternative import)
  - .png   weight/splat map per layer (Grass / Rock / Sand / Snow)
  - _preview.png  8-bit colored preview with hillshading
"""

import os
import struct
import logging
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, generic_filter

log = logging.getLogger(__name__)


# ─── Normalization ────────────────────────────────────────────────────────────

def normalize_to_ue5(
    elevation: np.ndarray,
    z_range_m: float = 400.0
) -> tuple:
    """
    Map real elevation (meters) → 0–65535 (uint16) for UE5.

    Stretches the full uint16 range to the actual min/max of the data,
    so even flat coastal terrain shows its subtle elevation variations.
    The actual elevation range is returned so UE5 Z scale can be set correctly.

    Returns:
        (encoded uint16 array, actual_min_m, actual_max_m)
    """
    e_min  = float(elevation.min())
    e_max  = float(elevation.max())
    e_range = e_max - e_min

    if e_range < 0.01:
        # Flat or failed fetch — flat terrain sitting at mid-level (actor Z = 0)
        log.warning(f"Elevation range ~0 ({e_range:.4f}m) — using flat mid-level heightmap")
        encoded = np.full(elevation.shape, 32768, dtype=np.uint16)
        return encoded, e_min, e_max

    normalized = (elevation - e_min) / e_range      # 0..1
    encoded    = (normalized * 65535.0).astype(np.uint16)
    return encoded, e_min, e_max


# ─── Hydraulic Erosion Simulation ────────────────────────────────────────────

def apply_hydraulic_erosion(
    elevation: np.ndarray,
    iterations: int = 3,
    rain_amount: float = 0.01,
    evaporation: float = 0.05,
    sediment_capacity: float = 0.01
) -> np.ndarray:
    """
    Simple iterative hydraulic erosion.
    Softens ridges and deposits sediment in valleys.
    For production use: pipe through World Machine / Gaea instead.
    """
    elev = elevation.astype(np.float32).copy()
    h, w = elev.shape

    log.info(f"Hydraulic erosion: {iterations} iterations on {h}×{w} grid")

    for iteration in range(iterations):
        # Laplacian-based flow: water flows to lowest neighbor
        kernel = np.array([[0, 1, 0],
                            [1, 0, 1],
                            [0, 1, 0]], dtype=np.float32) / 4.0

        local_avg = generic_filter(elev, np.mean, size=3)
        diff      = elev - local_avg

        # Erode high spots, deposit in low spots
        erosion_rate = np.clip(diff * rain_amount, 0, None)
        deposit_rate = np.clip(-diff * sediment_capacity, 0, None)

        elev -= erosion_rate
        elev += deposit_rate

        # Light smoothing to prevent artifacts
        elev = gaussian_filter(elev, sigma=0.3)

        log.debug(f"  Erosion pass {iteration+1}/{iterations}")

    return elev


# ─── Gaussian Smoothing ───────────────────────────────────────────────────────

def smooth_heightmap(elevation: np.ndarray, sigma: float = 3.0) -> np.ndarray:
    """Remove high-frequency noise from SRTM data artifacts."""
    return gaussian_filter(elevation.astype(np.float32), sigma=sigma)


# ─── Water Mask ──────────────────────────────────────────────────────────────

def apply_water_mask(
    elevation: np.ndarray,
    water_mask: np.ndarray,
    sea_elevation_m: float = -5.0
) -> np.ndarray:
    """
    Carve water pixels down to sea_elevation_m (default -5 m).
    Negative value ensures ocean body at Z=0 sits visibly above the sea floor.
    Combines the OSM flood-fill mask with a low-elevation threshold pass.
    """
    result     = elevation.copy().astype(np.float32)
    sea_pixels = water_mask | (elevation <= 2.0)
    result[sea_pixels] = sea_elevation_m
    return result


# ─── Slope / Normal Calculation ──────────────────────────────────────────────

def compute_slope(elevation: np.ndarray, cell_size_m: float = 1.0) -> np.ndarray:
    """Returns slope in degrees (0=flat, 90=vertical cliff)."""
    dz_dy, dz_dx = np.gradient(elevation.astype(np.float32), cell_size_m)
    slope_rad = np.arctan(np.sqrt(dz_dx**2 + dz_dy**2))
    return np.degrees(slope_rad)


def compute_hillshade(
    elevation: np.ndarray,
    sun_azimuth: float = 315.0,
    sun_altitude: float = 45.0,
    cell_size_m: float = 1.0
) -> np.ndarray:
    """Compute hillshade for preview visualization."""
    az_rad  = math.radians(360.0 - sun_azimuth + 90.0)
    alt_rad = math.radians(sun_altitude)

    dz_dy, dz_dx = np.gradient(elevation.astype(np.float32), cell_size_m)

    # Surface normal
    nx = -dz_dx
    ny = -dz_dy
    nz = np.ones_like(nx)
    norm = np.sqrt(nx**2 + ny**2 + nz**2)
    nx, ny, nz = nx/norm, ny/norm, nz/norm

    # Light direction
    lx = math.cos(alt_rad) * math.cos(az_rad)
    ly = math.cos(alt_rad) * math.sin(az_rad)
    lz = math.sin(alt_rad)

    shade = nx*lx + ny*ly + nz*lz
    shade = np.clip(shade, 0, 1)
    return shade


# ─── Splat / Weight Map Generation ───────────────────────────────────────────

def generate_splat_maps(
    elevation: np.ndarray,
    slope_deg: np.ndarray
) -> dict:
    """
    Generate per-layer weight maps for UE5 Landscape material blending.

    Layers:
      grass  — low elevation, gentle slope
      rock   — steep slope or high elevation
      sand   — flat, low elevation (near sea level)
      snow   — high elevation
    """
    elev_norm  = (elevation - elevation.min()) / max(float((elevation.max() - elevation.min())), 1.0)
    slope_norm = np.clip(slope_deg / 60.0, 0.0, 1.0)

    snow_line  = 0.75   # top 25% elevation
    sand_elev  = 0.10   # bottom 10% elevation

    snow  = np.clip((elev_norm - snow_line) / (1.0 - snow_line), 0, 1)
    sand  = np.clip((sand_elev - elev_norm) / sand_elev, 0, 1) * (1 - slope_norm)
    rock  = slope_norm * (1 - snow)
    grass = np.clip(1.0 - rock - snow - sand, 0.0, 1.0)

    # Normalize so layers sum to 1
    total = grass + rock + sand + snow + 1e-6
    return {
        "grass": (grass / total * 255).astype(np.uint8),
        "rock":  (rock  / total * 255).astype(np.uint8),
        "sand":  (sand  / total * 255).astype(np.uint8),
        "snow":  (snow  / total * 255).astype(np.uint8),
    }


# ─── Export Functions ─────────────────────────────────────────────────────────

def export_png_16bit(encoded: np.ndarray, path: str):
    """Save 16-bit grayscale PNG for UE5 Landscape import."""
    img = Image.fromarray(encoded, mode="I;16")
    img.save(path)
    log.info(f"Heightmap PNG saved: {path}  ({encoded.shape[1]}×{encoded.shape[0]})")


def export_r16(encoded: np.ndarray, path: str):
    """
    Save .r16 raw binary heightmap (little-endian uint16).
    UE5 Import: Landscape → From File → Raw (16-bit)
    """
    with open(path, "wb") as f:
        for row in encoded:
            for val in row:
                f.write(struct.pack("<H", int(val)))
    log.info(f"R16 raw heightmap saved: {path}")


def export_splat_map(weight_map: np.ndarray, path: str):
    """Save 8-bit grayscale splat/weight map."""
    img = Image.fromarray(weight_map, mode="L")
    img.save(path)


def export_preview(elevation: np.ndarray, path: str):
    """Export colored elevation preview with hillshading."""
    shade    = compute_hillshade(elevation)
    elev_n   = (elevation - elevation.min()) / max(float((elevation.max() - elevation.min())), 1.0)
    slope_n  = compute_slope(elevation)

    # Terrain color ramp (low=green, mid=brown, high=white)
    r = np.clip(elev_n * 1.2, 0, 1)
    g = np.clip(0.5 + elev_n * 0.5 - slope_n / 90.0, 0, 1)
    b = np.clip(elev_n * 0.8, 0, 1)

    rgb = np.stack([
        (r * shade * 255).astype(np.uint8),
        (g * shade * 255).astype(np.uint8),
        (b * shade * 255).astype(np.uint8),
    ], axis=-1)

    Image.fromarray(rgb, mode="RGB").save(path)
    log.info(f"Preview saved: {path}")


# ─── Master Export Pipeline ───────────────────────────────────────────────────

def process_and_export(
    elevation_raw:  np.ndarray,
    output_dir:     str,
    name:           str,
    z_range_m:      float = 400.0,
    smooth_sigma:   float = 3.0,
    apply_erosion:  bool  = False,
    erosion_iters:  int   = 3,
    water_mask:     np.ndarray = None
) -> dict:
    """
    Full processing pipeline: raw elevation → all UE5-ready assets.
    Returns dict of output file paths.
    """
    import math  # noqa — needed for hillshade inside subprocess context
    import builtins
    builtins.math = __import__("math")

    log.info(f"Processing heightmap '{name}'  shape={elevation_raw.shape}")

    # 1. Apply water mask before smoothing (flatten sea pixels to 0)
    if water_mask is not None:
        elevation_raw = apply_water_mask(elevation_raw, water_mask)

    # 2. Smooth SRTM noise
    elevation = smooth_heightmap(elevation_raw, sigma=smooth_sigma)

    # 3. Optional erosion simulation
    if apply_erosion:
        elevation = apply_hydraulic_erosion(elevation, iterations=erosion_iters)

    # 4. Compute slope for splat maps
    slope = compute_slope(elevation)

    # 4. Normalize → uint16 (stretch to actual min/max range)
    encoded, elev_min_m, elev_max_m = normalize_to_ue5(elevation, z_range_m=z_range_m)
    elev_range_m = elev_max_m - elev_min_m

    log.info(f"Elevation range: min={elev_min_m:.2f}m  max={elev_max_m:.2f}m  range={elev_range_m:.2f}m")

    # 5. Generate splat maps
    splat = generate_splat_maps(elevation, slope)

    # 6. Export all assets
    paths = {}
    paths["heightmap_png"] = os.path.join(output_dir, f"{name}_heightmap.png")
    paths["heightmap_r16"] = os.path.join(output_dir, f"{name}_heightmap.r16")
    paths["preview"]       = os.path.join(output_dir, f"{name}_preview.png")
    for layer in splat:
        paths[f"splat_{layer}"] = os.path.join(output_dir, f"{name}_splat_{layer}.png")

    export_png_16bit(encoded, paths["heightmap_png"])
    export_r16(encoded, paths["heightmap_r16"])
    export_preview(elevation, paths["preview"])
    for layer, wmap in splat.items():
        export_splat_map(wmap, paths[f"splat_{layer}"])

    paths["elevation_min_m"] = elev_min_m
    paths["elevation_max_m"] = elev_max_m
    paths["elevation_range_m"] = elev_range_m

    log.info(f"All assets exported to: {output_dir}")
    return paths


import math  # top-level import for hillshade functions used at module scope
