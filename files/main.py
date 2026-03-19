"""
Level Tool — Main CLI
Extraction Shooter Map Generator for UE5

Usage examples:

  # Landscape only (free SRTM source)
  python main.py landscape --preset Seoul_Jongno

  # Landscape with custom coordinates
  python main.py landscape --lat 37.5704 --lon 126.9820 --radius 1.0

  # Buildings only
  python main.py buildings --preset Seoul_Jongno

  # Full pipeline (landscape + buildings)
  python main.py all --preset Chernobyl

  # Full pipeline with Google API (higher quality elevation)
  python main.py all --preset Seoul_Jongno --elevation-source google --api-key YOUR_KEY

  # List available presets
  python main.py presets
"""

import sys
import os
import json
import argparse
import logging

# Force UTF-8 output on Windows (prevents cp949 UnicodeEncodeError)
if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if sys.stderr.encoding and sys.stderr.encoding.lower() != 'utf-8':
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

# Merge stderr into stdout so UE5 pipe capture gets everything
sys.stderr = sys.stdout

# Add project root to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from config import (
    COORD_PRESETS, HEIGHTMAP_SIZE, LANDSCAPE_Z_RANGE_METERS,
    LANDSCAPE_XY_SCALE, HEIGHTMAP_DIR, BUILDING_DIR, UE5_DIR,
    OUTPUT_DIR, WATER_DIR, GOOGLE_MAPS_API_KEY
)


# ─── Logging Setup ────────────────────────────────────────────────────────────

def setup_logging(verbose: bool = False):
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level  = level,
        format = "%(asctime)s  %(levelname)-7s  %(name)s  %(message)s",
        datefmt= "%H:%M:%S"
    )


# ─── Resolve Coordinates ──────────────────────────────────────────────────────

def resolve_coords(args):
    if args.preset:
        if args.preset not in COORD_PRESETS:
            print(f"ERROR: Unknown preset '{args.preset}'")
            print(f"Available: {', '.join(COORD_PRESETS)}")
            sys.exit(1)
        p = COORD_PRESETS[args.preset]
        name = getattr(args, "name", None) or args.preset
        return p["lat"], p["lon"], p["radius_km"], name
    elif args.lat and args.lon:
        name = getattr(args, "name", None) or f"custom_{args.lat:.3f}_{args.lon:.3f}"
        return args.lat, args.lon, args.radius, name
    else:
        print("ERROR: Provide --preset or --lat/--lon")
        sys.exit(1)


# ─── Command: Landscape ───────────────────────────────────────────────────────

def cmd_landscape(args):
    from landscape.elevation_fetcher import fetch_elevation
    from landscape.heightmap_exporter import process_and_export, apply_water_mask

    lat, lon, radius, name = resolve_coords(args)
    api_key = args.api_key or GOOGLE_MAPS_API_KEY
    grid_size = getattr(args, 'grid_size', 0) or HEIGHTMAP_SIZE

    print(f"\n{'='*55}")
    print(f"  Landscape Tool")
    print(f"  Location : {name}  ({lat:.4f}, {lon:.4f})")
    print(f"  Radius   : {radius} km")
    print(f"  Grid     : {grid_size} × {grid_size}")
    print(f"  Source   : {args.elevation_source}")
    print(f"{'='*55}\n")

    # Fetch elevation
    elevation = fetch_elevation(
        center_lat  = lat,
        center_lon  = lon,
        radius_km   = radius,
        grid_size   = grid_size,
        source      = args.elevation_source,
        api_key     = api_key
    )

    print(f"Elevation stats : min={elevation.min():.2f}m  max={elevation.max():.2f}m  "
          f"mean={elevation.mean():.2f}m  range={elevation.max() - elevation.min():.2f}m")

    # Fetch water mask (separate deep/river masks)
    water_path = os.path.join(WATER_DIR, f"{name}_water.json")
    deep_mask_arr  = None
    river_mask_arr = None
    if not getattr(args, "no_water", False):
        try:
            from landscape.water_mask import build_water_mask
            deep_mask_arr, river_mask_arr, _ = build_water_mask(
                center_lat       = lat,
                center_lon       = lon,
                radius_km        = radius,
                grid_h           = grid_size,
                grid_w           = grid_size,
                output_json_path = water_path
            )
            combined = (deep_mask_arr | river_mask_arr) if river_mask_arr is not None else deep_mask_arr
            if combined.any():
                print(f"Water mask      : deep={100*deep_mask_arr.mean():.1f}%  "
                      f"river={100*river_mask_arr.mean():.1f}%")
            else:
                print(f"Water mask      : no water features found in area")
                water_path = ""
        except Exception as e:
            logging.getLogger(__name__).warning(f"Water mask skipped: {e}")
            water_path = ""

    # Detect terrain type from elevation profile + water coverage
    terrain_type = getattr(args, "terrain_type", "auto")
    if terrain_type == "auto":
        elev_range = float(elevation.max() - elevation.min())
        deep_pct = float(deep_mask_arr.mean()) * 100 if deep_mask_arr is not None else 0.0

        if deep_pct > 50.0:
            terrain_type = "island"
        elif elev_range < 100.0:
            terrain_type = "urban"
        else:
            terrain_type = "natural"
        print(f"Terrain type    : {terrain_type} (auto, range={elev_range:.0f}m, water={deep_pct:.1f}%)")

    # Process and export
    paths = process_and_export(
        elevation_raw  = elevation,
        output_dir     = HEIGHTMAP_DIR,
        name           = name,
        z_range_m      = LANDSCAPE_Z_RANGE_METERS,
        smooth_sigma   = args.smooth_sigma,
        apply_erosion  = not args.no_erosion,
        erosion_iters  = args.erosion_iters,
        water_mask     = deep_mask_arr,
        river_mask     = river_mask_arr,
        terrain_type   = terrain_type,
        radius_km      = radius
    )

    print(f"\nOutputs:")
    for key, path in paths.items():
        if isinstance(path, str):
            print(f"  {key:20s} → {os.path.basename(path)}")

    elev_range = paths.get("elevation_range_m", LANDSCAPE_Z_RANGE_METERS)
    z_scale    = elev_range * 100.0 / 512.0
    print(f"Elevation min   : {paths.get('elevation_min_m', 0.0):.2f}m")
    print(f"Elevation max   : {paths.get('elevation_max_m', 0.0):.2f}m")
    print(f"Elevation range : {elev_range:.2f}m")
    print(f"Heightmap PNG   : {paths['heightmap_png']}")
    if water_path and os.path.exists(water_path):
        print(f"Water JSON      : {water_path}")
    print(f"\nUE5 Import:")
    print(f"  1. Open your map in Unreal Editor")
    print(f"  2. Mode: Landscape → New Landscape → Import from File")
    print(f"  3. File: {paths['heightmap_png']}")
    print(f"  4. Scale XY: {LANDSCAPE_XY_SCALE}  |  Scale Z: {z_scale:.4f}")
    print()

    result = {
        "heightmap_png": paths['heightmap_png'],
        "heightmap_r16": paths.get('heightmap_r16', ''),
        "elevation_min_m": float(paths.get('elevation_min_m', 0.0)),
        "elevation_max_m": float(paths.get('elevation_max_m', 0.0)),
    }
    print(f"__LEVELTOOL_RESULT__:{json.dumps(result)}")


# ─── Command: Buildings ──────────────────────────────────────────────────────

def cmd_buildings(args):
    from buildings.osm_fetcher import fetch_and_parse_buildings

    lat, lon, radius, name = resolve_coords(args)

    print(f"\n{'='*55}")
    print(f"  Building Tool")
    print(f"  Location : {name}  ({lat:.4f}, {lon:.4f})")
    print(f"  Radius   : {radius} km")
    print(f"{'='*55}\n")

    json_path  = os.path.join(BUILDING_DIR, f"{name}_buildings.json")
    roads_path = os.path.join(BUILDING_DIR, f"{name}_roads.json")

    buildings = fetch_and_parse_buildings(
        center_lat       = lat,
        center_lon       = lon,
        radius_km        = radius,
        output_json_path = json_path,
        min_area_m2      = 20.0,
        default_height_m = 10.0
    )

    # Road network (non-fatal: skip on failure)
    if not getattr(args, "no_roads", False):
        from roads.osm_roads import fetch_and_parse_roads
        fetch_and_parse_roads(lat, lon, radius, roads_path)

    # Stats
    type_counts = {}
    for b in buildings:
        type_counts[b["type"]] = type_counts.get(b["type"], 0) + 1

    heights = [b["height_m"] for b in buildings]
    print(f"\nBuilding statistics:")
    print(f"  Total  : {len(buildings)}")
    if heights:
        print(f"  Height : min={min(heights):.1f}m  max={max(heights):.1f}m  avg={sum(heights)/len(heights):.1f}m")
    print(f"\n  By type:")
    for t, c in sorted(type_counts.items(), key=lambda x: -x[1]):
        print(f"    {c:4d} × {t}")

    # Generate PCG CSV
    pcg_csv = os.path.join(UE5_DIR, f"{name}_pcg.csv")
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "ue5"))
    from ue5.level_builder import generate_pcg_building_data
    generate_pcg_building_data(json_path, pcg_csv)

    print(f"\nOutputs:")
    print(f"  Buildings JSON : {json_path}")
    print(f"  Roads JSON     : {roads_path}")
    print(f"  PCG CSV        : {pcg_csv}")

    result = {
        "buildings_json": json_path,
        "roads_json": roads_path,
    }
    print(f"__LEVELTOOL_RESULT__:{json.dumps(result)}")

    print(f"\nNext steps:")
    print(f"  Blender FBX export:")
    print(f"    blender --background --python buildings/blender_extrude.py -- {json_path}")
    print(f"\n  UE5 Python (run in Unreal Editor Python console):")
    print(f"    import sys; sys.path.insert(0, 'YOUR_TOOL_PATH')")
    print(f"    from ue5.level_builder import place_buildings")
    print(f"    place_buildings(r'{json_path}')")
    print()


# ─── Command: All ────────────────────────────────────────────────────────────

def cmd_ai_textures(args):
    """Phase 4a: SD texture generation for all building types."""
    from ai.ai_pipeline import run_ai_pipeline

    buildings_json = args.buildings
    if not buildings_json:
        # Try to find the most recent buildings JSON
        import glob
        pattern = os.path.join(BUILDING_DIR, "*_buildings.json")
        matches = sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True)
        if not matches:
            print("ERROR: No buildings JSON found. Run 'buildings' command first, "
                  "or specify --buildings <path>.")
            sys.exit(1)
        buildings_json = matches[0]
        print(f"Using most recent buildings file: {os.path.basename(buildings_json)}")

    if not os.path.exists(buildings_json):
        print(f"ERROR: File not found: {buildings_json}")
        sys.exit(1)

    output_dir = args.ai_output_dir or os.path.join(OUTPUT_DIR, "ai")

    print(f"\n{'='*55}")
    print(f"  Phase 4 — AI Enhancement Pipeline")
    print(f"  Buildings : {os.path.basename(buildings_json)}")
    print(f"  Mode      : {args.ai_mode}")
    print(f"  SD backend: {args.sd_backend}")
    print(f"  Tex size  : {args.texture_size}px")
    if args.ai_mode in ("meshes", "full"):
        print(f"  Mesh res  : {args.mesh_resolution}")
    print(f"  Output    : {output_dir}")
    print(f"{'='*55}\n")

    results = run_ai_pipeline(
        buildings_json_path = buildings_json,
        output_dir          = output_dir,
        mode                = args.ai_mode,
        sd_backend          = args.sd_backend,
        sd_api_key          = args.sd_api_key or "",
        sd_model_id         = args.sd_model_id or "",
        texture_size        = args.texture_size,
        mesh_resolution     = args.mesh_resolution,
        device              = args.device,
        overwrite           = args.overwrite,
    )

    n_tex   = sum(len(v) for v in results.get("textures", {}).values())
    n_mesh  = len(results.get("meshes", {}))

    print(f"\nOutputs:")
    if results.get("textures"):
        tex_dir = os.path.join(output_dir, "textures")
        print(f"  Textures ({n_tex} sets) → {tex_dir}")
        print(f"  Manifest               → {os.path.join(tex_dir, 'texture_manifest.json')}")
    if results.get("meshes"):
        mesh_dir = os.path.join(output_dir, "meshes")
        print(f"  Meshes   ({n_mesh})     → {mesh_dir}")
    print()


def cmd_all(args):
    cmd_landscape(args)

    try:
        cmd_buildings(args)
    except Exception as e:
        print(f"\n⚠ Building fetch failed (non-fatal): {e}")
        print("  Landscape was generated successfully. Retry buildings separately.\n")

    lat, lon, radius, name = resolve_coords(args)
    hm_path   = os.path.join(HEIGHTMAP_DIR, f"{name}_heightmap.png")
    bldg_path = os.path.join(BUILDING_DIR,  f"{name}_buildings.json")

    roads_path = os.path.join(BUILDING_DIR, f"{name}_roads.json")

    print(f"\n{'='*55}")
    print(f"  UE5 Full Pipeline Command")
    print(f"{'='*55}")
    print(f"  # Run in UE5 Python console:")
    print(f"  import sys")
    print(f"  sys.path.insert(0, r'YOUR_LEVEL_TOOL_PATH')")
    print(f"  from ue5.level_builder import run_full_pipeline")
    print(f"  run_full_pipeline(")
    print(f"      heightmap_path = r'{hm_path}',")
    print(f"      buildings_json = r'{bldg_path}',")
    print(f"      map_name       = '{name}',")
    print(f"      xy_scale       = {LANDSCAPE_XY_SCALE},")
    print(f"      z_range_cm     = {int(LANDSCAPE_Z_RANGE_METERS * 100)},")
    print(f"  )")
    print()

    combined_result = {
        "heightmap_png": hm_path,
        "buildings_json": bldg_path,
        "roads_json": roads_path,
        "elevation_min_m": 0.0,
        "elevation_max_m": 0.0,
    }
    print(f"__LEVELTOOL_RESULT__:{json.dumps(combined_result)}")

    # Phase 4 AI enhancement (optional)
    if getattr(args, "ai_enhance", False):
        args.buildings    = bldg_path
        args.ai_mode      = getattr(args, "ai_mode", "textures")
        args.ai_output_dir = None
        cmd_ai_textures(args)


# ─── Command: Presets ────────────────────────────────────────────────────────

def cmd_presets(args):
    print("\nAvailable coordinate presets:\n")
    for name, p in COORD_PRESETS.items():
        print(f"  {name:25s}  lat={p['lat']:.4f}  lon={p['lon']:.4f}  radius={p['radius_km']}km")
    print()


# ─── Argument Parser ──────────────────────────────────────────────────────────

def build_parser():
    parser = argparse.ArgumentParser(
        prog="level_tool",
        description="UE5 Level Generation Tool — Extraction Shooter Map Generator"
    )
    parser.add_argument("--verbose", action="store_true")

    subs = parser.add_subparsers(dest="command", required=True)

    # Shared AI args (used by 'all' and 'ai-textures')
    def add_ai_args(p):
        p.add_argument("--ai-mode",         default="textures",
                       choices=["textures", "meshes", "full"],
                       help="AI steps: textures / meshes / full (default: textures)")
        p.add_argument("--sd-backend",      default="local", choices=["local", "api"],
                       help="SD inference backend (default: local)")
        p.add_argument("--sd-api-key",      type=str, default="",
                       help="Stability AI API key (api backend only)")
        p.add_argument("--sd-model-id",     type=str, default="",
                       help="HuggingFace model ID (local backend)")
        p.add_argument("--texture-size",    type=int, default=512,
                       help="Texture resolution in pixels (default: 512)")
        p.add_argument("--mesh-resolution", type=int, default=256,
                       help="TripoSR Marching Cubes resolution (default: 256)")
        p.add_argument("--device",          type=str, default=None,
                       help="Torch device: cuda / cpu (default: auto)")
        p.add_argument("--overwrite",       action="store_true",
                       help="Regenerate assets even if they already exist")

    # Shared coordinate args
    def add_coord_args(p):
        grp = p.add_mutually_exclusive_group()
        grp.add_argument("--preset",  type=str, help="Use named coordinate preset (optional, config.py)")
        p.add_argument("--lat",    type=float, help="Center latitude")
        p.add_argument("--lon",    type=float, help="Center longitude")
        p.add_argument("--radius", type=float, default=1.0, help="Radius in km (default: 1.0)")
        p.add_argument("--name",   type=str,   default="",  help="Map name used for output filenames")
        p.add_argument("--grid-size", type=int, default=0,
                       help="Heightmap grid size (0 = use config.py default)")

    # landscape
    lp = subs.add_parser("landscape", help="Generate heightmap from elevation data")
    add_coord_args(lp)
    lp.add_argument("--elevation-source", default="open_elevation",
                    choices=["open_elevation", "opentopography", "google"],
                    help="Elevation data source (default: open_elevation)")
    lp.add_argument("--api-key",      type=str, default="",    help="API key (Google/OpenTopography)")
    lp.add_argument("--smooth-sigma", type=float, default=3.0, help="Gaussian smooth sigma (default: 3.0)")
    lp.add_argument("--no-erosion",   action="store_true",     help="Skip erosion simulation")
    lp.add_argument("--erosion-iters",type=int, default=3,     help="Erosion iterations (default: 3)")
    lp.add_argument("--no-water",     action="store_true",     help="Skip water mask fetch")
    lp.add_argument("--terrain-type", default="auto",
                    choices=["auto", "urban", "natural"],
                    help="Terrain type: auto-detect, urban (gentle), natural (full) (default: auto)")
    lp.set_defaults(func=cmd_landscape)

    # buildings
    bp = subs.add_parser("buildings", help="Fetch OSM buildings and generate placement data")
    add_coord_args(bp)
    bp.add_argument("--no-roads", action="store_true", help="Skip road data fetch")
    bp.set_defaults(func=cmd_buildings)

    # all
    ap = subs.add_parser("all", help="Run complete pipeline (landscape + buildings)")
    add_coord_args(ap)
    ap.add_argument("--elevation-source", default="open_elevation",
                    choices=["open_elevation", "opentopography", "google"])
    ap.add_argument("--api-key",      type=str, default="")
    ap.add_argument("--smooth-sigma", type=float, default=3.0)
    ap.add_argument("--no-erosion",   action="store_true")
    ap.add_argument("--erosion-iters",type=int, default=3)
    ap.add_argument("--no-roads",     action="store_true", help="Skip road data fetch")
    ap.add_argument("--no-water",     action="store_true", help="Skip water mask fetch")
    ap.add_argument("--terrain-type", default="auto",
                    choices=["auto", "urban", "natural"])
    ap.add_argument("--ai-enhance",   action="store_true",
                    help="Run Phase 4 AI enhancement (SD textures) after buildings")
    add_ai_args(ap)
    ap.set_defaults(func=cmd_all)

    # ai-textures  (Phase 4 standalone)
    aip = subs.add_parser(
        "ai-textures",
        help="Phase 4 — AI texture/mesh generation for an existing buildings JSON"
    )
    aip.add_argument("--buildings", type=str, default="",
                     help="Path to *_buildings.json  (default: most recent in output/buildings/)")
    aip.add_argument("--ai-output-dir", type=str, default="",
                     help="Root output dir for textures/ and meshes/")
    add_ai_args(aip)
    aip.set_defaults(func=cmd_ai_textures)

    # presets
    pp = subs.add_parser("presets", help="List available coordinate presets")
    pp.set_defaults(func=cmd_presets)

    return parser


def main():
    parser = build_parser()
    args   = parser.parse_args()
    setup_logging(args.verbose)
    args.func(args)


if __name__ == "__main__":
    main()
