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
from scipy.ndimage import gaussian_filter, uniform_filter

log = logging.getLogger(__name__)


# ─── Normalization ────────────────────────────────────────────────────────────

def normalize_to_ue5(
    elevation: np.ndarray,
    z_range_m: float = 400.0
) -> tuple:
    """
    Map real elevation (meters) → 0–65535 (uint16) for UE5.

    Uses percentile-based clamping when extreme outliers would otherwise
    compress the usable uint16 range, preserving fine detail in the
    terrain body.

    Returns:
        (encoded uint16 array, effective_min_m, effective_max_m)
    """
    e_abs_min   = float(elevation.min())
    e_abs_max   = float(elevation.max())
    e_abs_range = e_abs_max - e_abs_min

    if e_abs_range < 0.01:
        log.warning(f"Elevation range ~0 ({e_abs_range:.4f}m) — using flat mid-level heightmap")
        encoded = np.full(elevation.shape, 32768, dtype=np.uint16)
        return encoded, e_abs_min, e_abs_max

    e_low   = float(np.percentile(elevation, 0.5))
    e_high  = float(np.percentile(elevation, 99.5))
    p_range = e_high - e_low

    if p_range >= 0.01 and (e_abs_range / p_range) > 1.5:
        log.info(f"Percentile clamp: [{e_low:.1f}, {e_high:.1f}]m "
                 f"(full [{e_abs_min:.1f}, {e_abs_max:.1f}]m)")
        clipped    = np.clip(elevation, e_low, e_high)
        normalized = (clipped - e_low) / p_range
        encoded    = (normalized * 65535.0).astype(np.uint16)
        return encoded, e_low, e_high

    normalized = (elevation - e_abs_min) / e_abs_range
    encoded    = (normalized * 65535.0).astype(np.uint16)
    return encoded, e_abs_min, e_abs_max


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

        local_avg = uniform_filter(elev, size=3)
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
    sea_elevation_m: float = -5.0,
    sea_threshold_m: float = 2.0,
    river_mask: np.ndarray = None,
    river_depth_m: float = 1.5
) -> np.ndarray:
    """
    Carve water into terrain with separate depths for ocean/lakes and rivers.

    - Ocean/lakes (deep_mask): flatten to sea_elevation_m
    - Rivers (river_mask): carve river_depth_m below surrounding terrain
    """
    result = elevation.copy().astype(np.float32)

    # Only flatten pixels that are BOTH in the water mask AND have low elevation.
    # High-elevation pixels (>10m) marked as water by flood-fill are land
    # (coastline flood-fill artifacts on mountains/cliffs).
    land_elev_guard = 10.0
    sea_pixels = (water_mask & (elevation <= land_elev_guard)) | (elevation <= sea_threshold_m)
    result[sea_pixels] = sea_elevation_m

    if river_mask is not None:
        river_only = river_mask & ~sea_pixels
        if river_only.any():
            surrounding = uniform_filter(elevation, size=7)
            result[river_only] = surrounding[river_only] - river_depth_m
            log.info(f"  River carve: {int(river_only.sum())} pixels at -{river_depth_m}m relative")

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

    snow_line = max(float(np.percentile(elev_norm, 80)), 0.50)
    sand_elev = min(float(np.percentile(elev_norm, 15)), snow_line * 0.3)
    sand_elev = max(sand_elev, 0.02)

    snow  = np.clip((elev_norm - snow_line) / max(1.0 - snow_line, 0.01), 0, 1)
    sand  = np.clip((sand_elev - elev_norm) / max(sand_elev, 0.01), 0, 1) * (1 - slope_norm)
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
        f.write(encoded.astype(np.uint16).tobytes())
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
    elevation_raw:   np.ndarray,
    output_dir:      str,
    name:            str,
    z_range_m:       float = 400.0,
    smooth_sigma:    float = 3.0,
    apply_erosion:   bool  = False,
    erosion_iters:   int   = 3,
    water_mask:      np.ndarray = None,
    sea_threshold_m: float = 2.0,
    river_mask:      np.ndarray = None,
    river_depth_m:   float = 1.5,
    terrain_type:    str   = "natural",
    radius_km:       float = 1.0
) -> dict:
    """
    Full processing pipeline: raw elevation → all UE5-ready assets.

    terrain_type: "natural" (full erosion/smoothing) or "urban" (gentle processing).
    Returns dict of output file paths.
    """
    import math  # noqa — needed for hillshade inside subprocess context
    import builtins
    builtins.math = __import__("math")

    log.info(f"Processing heightmap '{name}'  shape={elevation_raw.shape}  terrain={terrain_type}")

    # 1. Apply water mask (ocean/lake deep carve + river shallow carve)
    if water_mask is not None:
        elevation_raw = apply_water_mask(elevation_raw, water_mask,
                                         sea_threshold_m=sea_threshold_m,
                                         river_mask=river_mask,
                                         river_depth_m=river_depth_m)

    # 2. DSM → DTM: remove building rooftop spikes from SRTM/OpenTopo data.
    #    SRTM is a DSM that includes building heights as "terrain".
    #    We detect spikes by comparing each pixel to a local minimum baseline;
    #    natural mountains have gradual slopes so spike height stays low,
    #    whereas buildings create sharp vertical jumps of 10-50+ m.
    from scipy.ndimage import minimum_filter, median_filter

    elev_f = elevation_raw.astype(np.float32)
    h, w = elev_f.shape
    raw_range = float(elev_f.max() - elev_f.min())
    log.info(f"  DSM→DTM: raw elevation range={raw_range:.1f}m  terrain_type={terrain_type}")

    if terrain_type == "island":
        # Island mode: most of the area is ocean (-5m). DSM→DTM would destroy
        # the island's tiny land mass. Only apply gentle smoothing to land pixels.
        land_mask = elev_f > (sea_threshold_m + 0.5)
        n_land = int(land_mask.sum())
        log.info(f"  Island mode: land={n_land} pixels ({n_land/(h*w)*100:.1f}%), "
                 f"skip DSM→DTM")

        effective_sigma = min(smooth_sigma, 2.0)
        elevation = smooth_heightmap(elev_f, sigma=effective_sigma)
    elif terrain_type == "urban":
        # Aggressive: whole area is urban, replace elevation with ground baseline
        min_win = max(h // 14, 101)
        base = minimum_filter(elev_f, size=min_win)
        blur_sigma = max(h // 20, 50)
        base = gaussian_filter(base.astype(np.float32), sigma=blur_sigma)
        base_range = float(base.max() - base.min())
        log.info(f"  Urban DSM→DTM: min_filter({min_win}) + gauss({blur_sigma})  "
                 f"range={base_range:.1f}m")

        max_urban_range_m = 30.0
        if base_range > max_urban_range_m:
            base_floor = float(np.percentile(base, 5))
            above = base - base_floor
            unity_m = max_urban_range_m * 0.5
            elevation = np.where(
                above > 0,
                base_floor + unity_m * np.log1p(above / unity_m),
                base)
            log.info(f"  Urban DSM→DTM: log-compress {base_range:.1f}m → "
                     f"{float(elevation.max()-elevation.min()):.1f}m")
        else:
            elevation = base

        effective_sigma = max(smooth_sigma, 8.0)
        elevation = smooth_heightmap(elevation, sigma=effective_sigma)
    else:
        # Natural/mixed terrain: morphological opening removes features smaller
        # than the structuring element (buildings ~20-60m) while preserving
        # larger features (mountains, hills, ridges).
        from scipy.ndimage import maximum_filter

        diameter_m = radius_km * 2.0 * 1000.0
        pixel_m = diameter_m / max(w, 1)
        # Remove structures up to ~80m footprint (covers most buildings)
        target_m = 80.0
        opening_win = max(int(target_m / max(pixel_m, 0.5)), 15)
        if opening_win % 2 == 0:
            opening_win += 1
        opening_win = min(opening_win, 81)

        log.info(f"  Morphological opening: window={opening_win}px "
                 f"(≈{opening_win * pixel_m:.0f}m)")

        # Opening = erosion (min) + dilation (max)
        opened = maximum_filter(minimum_filter(elev_f, size=opening_win),
                                size=opening_win)

        # Spike = positive residual above the opened surface
        spike_height = elev_f - opened
        spike_thresh = 10.0
        spike_mask = spike_height > spike_thresh

        n_spike = int(spike_mask.sum())
        pct_spike = n_spike / (h * w) * 100
        log.info(f"  DSM spike detect: threshold={spike_thresh}m, "
                 f"spikes={n_spike} ({pct_spike:.1f}%)")

        if n_spike > 0 and pct_spike < 30.0:
            # Blend spike areas toward the opened surface
            blend_mask = gaussian_filter(spike_mask.astype(np.float32), sigma=5)
            blend_mask = np.clip(blend_mask, 0, 1)
            corrected = opened + np.clip(spike_height, 0, spike_thresh)
            elevation = elev_f * (1 - blend_mask) + corrected * blend_mask
            log.info(f"  DSM spike removal: {n_spike} pixels corrected, "
                     f"range {float(elevation.max()-elevation.min()):.1f}m")
        else:
            elevation = elev_f
            if pct_spike >= 30.0:
                log.info(f"  DSM spike skip: {pct_spike:.1f}% above threshold — "
                         f"likely steep terrain, not buildings")
            else:
                log.info(f"  DSM spike detect: no spikes found")

        effective_sigma = smooth_sigma
        elevation = smooth_heightmap(elevation, sigma=effective_sigma)

    log.info(f"  Smoothing: sigma={effective_sigma:.1f} (requested {smooth_sigma:.1f})")

    # 3. Optional erosion — skip entirely for urban terrain
    if apply_erosion:
        if terrain_type == "urban":
            pass  # no erosion in cities
        else:
            elevation = apply_hydraulic_erosion(elevation, iterations=erosion_iters)

    # 3b. Non-linear mountain exaggeration:
    #     Flat/urban areas stay unchanged; only elevations above the
    #     "mountain threshold" get their excess height scaled up.
    mtn_exag = 1.8
    elev_f32 = elevation.astype(np.float32)
    elev_range_pre = float(elev_f32.max() - elev_f32.min())

    if elev_range_pre > 50.0 and mtn_exag > 1.0:
        mtn_start = float(np.percentile(elev_f32, 70))
        excess = np.maximum(elev_f32 - mtn_start, 0.0)
        max_excess = float(excess.max())

        if max_excess > 10.0:
            elevation = elev_f32 + excess * (mtn_exag - 1.0)
            new_range = float(elevation.max() - elevation.min())
            log.info(f"  Mountain exaggeration: threshold={mtn_start:.1f}m, "
                     f"factor={mtn_exag}x, range {elev_range_pre:.1f}m → {new_range:.1f}m")

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

    # 6b. Generate composite colormap from splat weights for landscape material
    grass_color = np.array([0.35, 0.50, 0.25])   # muted green
    rock_color  = np.array([0.45, 0.42, 0.38])   # warm gray
    sand_color  = np.array([0.72, 0.65, 0.50])   # sandy tan
    snow_color  = np.array([0.90, 0.90, 0.92])   # near-white

    h, w = elevation.shape
    colormap = np.zeros((h, w, 3), dtype=np.float32)
    total = (splat["grass"].astype(np.float32) + splat["rock"].astype(np.float32)
             + splat["sand"].astype(np.float32) + splat["snow"].astype(np.float32) + 1e-6)
    for color, layer_name in [(grass_color, "grass"), (rock_color, "rock"),
                               (sand_color, "sand"), (snow_color, "snow")]:
        weight = splat[layer_name].astype(np.float32) / total
        colormap += weight[:, :, np.newaxis] * color[np.newaxis, np.newaxis, :]

    colormap_path = os.path.join(output_dir, f"{name}_colormap.png")
    Image.fromarray((np.clip(colormap, 0, 1) * 255).astype(np.uint8)).save(colormap_path)
    paths["colormap"] = colormap_path
    log.info(f"Colormap saved: {colormap_path}")

    paths["elevation_min_m"] = elev_min_m
    paths["elevation_max_m"] = elev_max_m
    paths["elevation_range_m"] = elev_range_m

    log.info(f"All assets exported to: {output_dir}")
    return paths


import math  # top-level import for hillshade functions used at module scope
