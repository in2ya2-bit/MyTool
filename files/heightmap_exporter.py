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
) -> np.ndarray:
    """
    Map real elevation (meters) → 0–65535 (uint16) for UE5.

    UE5 heightmap encoding:
      value 32768 = "sea level" (mid point)
      full range = z_range_m meters total
    """
    # Center at mean elevation
    center = elevation.mean()
    half   = z_range_m / 2.0

    clipped = np.clip(elevation, center - half, center + half)
    normalized = (clipped - (center - half)) / z_range_m  # 0..1
    encoded = (normalized * 65535.0).astype(np.uint16)
    return encoded


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

def smooth_heightmap(elevation: np.ndarray, sigma: float = 1.5) -> np.ndarray:
    """Remove high-frequency noise from SRTM data artifacts."""
    return gaussian_filter(elevation.astype(np.float32), sigma=sigma)


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
    smooth_sigma:   float = 1.5,
    apply_erosion:  bool  = True,
    erosion_iters:  int   = 3
) -> dict:
    """
    Full processing pipeline: raw elevation → all UE5-ready assets.
    Returns dict of output file paths.
    """
    import math  # noqa — needed for hillshade inside subprocess context
    import builtins
    builtins.math = __import__("math")

    log.info(f"Processing heightmap '{name}'  shape={elevation_raw.shape}")

    # 1. Smooth SRTM noise
    elevation = smooth_heightmap(elevation_raw, sigma=smooth_sigma)

    # 2. Optional erosion simulation
    if apply_erosion:
        elevation = apply_hydraulic_erosion(elevation, iterations=erosion_iters)

    # 3. Compute slope for splat maps
    slope = compute_slope(elevation)

    # 4. Normalize → uint16
    encoded = normalize_to_ue5(elevation, z_range_m=z_range_m)

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

    log.info(f"All assets exported to: {output_dir}")
    return paths


import math  # top-level import for hillshade functions used at module scope
