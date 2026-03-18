"""
AI Enhancement Pipeline
Phase 4 — TripoSR + Stable Diffusion

Orchestrates:
  1. SD PBR texture generation (wall + roof per building type)
  2. TripoSR mesh generation (facade image → 3D mesh)
  3. Injects ai_textures / ai_mesh fields into buildings JSON

Modes:
  textures  — SD texture generation only
  meshes    — TripoSR mesh generation only (textures must exist)
  full      — SD textures + TripoSR meshes

Usage (from Python):
  from ai.ai_pipeline import run_ai_pipeline
  run_ai_pipeline(buildings_json, output_dir, mode="textures")

Usage (CLI):
  python ai/ai_pipeline.py --buildings output/buildings/Seoul_buildings.json
  python ai/ai_pipeline.py --buildings ... --mode full --sd-backend api --sd-api-key KEY
"""

import os
import json
import logging
from typing import Dict, Optional

log = logging.getLogger(__name__)


# ─── Main Entry Point ─────────────────────────────────────────────────────────

def run_ai_pipeline(
    buildings_json_path: str,
    output_dir:          str,
    mode:                str  = "textures",   # "textures" | "meshes" | "full"
    sd_backend:          str  = "local",       # "local" | "api"
    sd_api_key:          str  = "",
    sd_model_id:         str  = "",
    texture_size:        int  = 512,
    mesh_resolution:     int  = 256,
    device:              Optional[str] = None,
    overwrite:           bool = False,
) -> Dict:
    """
    Run Phase 4 AI enhancement on a generated buildings JSON.

    Args:
        buildings_json_path: Path to *_buildings.json from osm_fetcher
        output_dir:          Root output directory (textures/ and meshes/ created here)
        mode:                Which AI steps to run (textures / meshes / full)
        sd_backend:          SD inference backend ("local" or "api")
        sd_api_key:          Stability AI API key (api backend only)
        sd_model_id:         HuggingFace model ID (local backend, default SD 2.1)
        texture_size:        Texture resolution in pixels (512 / 1024)
        mesh_resolution:     TripoSR Marching Cubes resolution (128 / 256 / 512)
        device:              Torch device override ("cuda" / "cpu" / None = auto)
        overwrite:           Regenerate assets even if they already exist

    Returns:
        {
          "textures": { building_type: { surface: { albedo/normal/roughness: path } } },
          "meshes":   { building_type: obj_path },
        }
    """
    import sys
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    texture_dir = os.path.join(output_dir, "textures")
    mesh_dir    = os.path.join(output_dir, "meshes")
    results: Dict = {}

    # Load buildings to report stats
    with open(buildings_json_path) as f:
        buildings = json.load(f)

    types_present = sorted({b["type"] for b in buildings})
    log.info(f"Buildings: {len(buildings)} total  |  {len(types_present)} types")
    log.info(f"Types: {', '.join(types_present)}")
    log.info(f"AI mode: {mode}")

    # ── Step 1: SD Texture Generation ────────────────────────────────────────
    if mode in ("textures", "full"):
        log.info("\n─── Phase 4a: SD Texture Generation ───────────────────────────")
        from ai.sd_texture_gen import generate_all_textures

        results["textures"] = generate_all_textures(
            output_dir   = texture_dir,
            backend_type = sd_backend,
            api_key      = sd_api_key,
            model_id     = sd_model_id,
            size         = texture_size,
            surfaces     = ["wall", "roof"],
            overwrite    = overwrite,
        )
        log.info(f"  ✔ Textures: {texture_dir}")

    # ── Step 2: TripoSR Mesh Generation ──────────────────────────────────────
    if mode in ("meshes", "full"):
        log.info("\n─── Phase 4b: TripoSR Mesh Generation ─────────────────────────")
        manifest_path = os.path.join(texture_dir, "texture_manifest.json")

        if not os.path.exists(manifest_path):
            log.error(f"Texture manifest not found: {manifest_path}")
            log.error("  Run with mode='textures' or 'full' first, then retry meshes.")
            results["meshes"] = {}
        else:
            from ai.triposr_mesh_gen import TripoSRGenerator, generate_building_meshes

            generator = TripoSRGenerator(device=device)
            results["meshes"] = generate_building_meshes(
                texture_manifest_path = manifest_path,
                output_dir            = mesh_dir,
                generator             = generator,
                mc_resolution         = mesh_resolution,
            )
            log.info(f"  ✔ Meshes: {mesh_dir}")

    # ── Step 3: Inject AI results back into buildings JSON ───────────────────
    _inject_ai_results(buildings_json_path, results)

    # ── Summary ───────────────────────────────────────────────────────────────
    n_textures = sum(len(v) for v in results.get("textures", {}).values())
    n_meshes   = len(results.get("meshes", {}))
    log.info(
        f"\n=== Phase 4 Complete ===  "
        f"texture sets={n_textures}  meshes={n_meshes}"
    )
    return results


# ─── Inject AI Results into Buildings JSON ───────────────────────────────────

def _inject_ai_results(buildings_json_path: str, ai_results: dict):
    """
    Add ai_textures / ai_mesh fields to every building entry so that
    the UE5 import pipeline can locate generated assets automatically.
    """
    with open(buildings_json_path) as f:
        buildings = json.load(f)

    textures = ai_results.get("textures", {})
    meshes   = ai_results.get("meshes", {})
    changed  = 0

    for b in buildings:
        btype = b["type"]
        if btype in textures:
            b["ai_textures"] = textures[btype]
            changed += 1
        if btype in meshes:
            b["ai_mesh"] = meshes[btype]

    with open(buildings_json_path, "w", encoding="utf-8") as f:
        json.dump(buildings, f, indent=2, ensure_ascii=False)

    log.info(f"Injected AI results into {changed}/{len(buildings)} buildings: {buildings_json_path}")


# ─── Convenience Wrappers ─────────────────────────────────────────────────────

def run_texture_pipeline(
    buildings_json_path: str,
    output_dir:          str,
    backend:             str = "local",
    api_key:             str = "",
    model_id:            str = "",
    size:                int = 512,
    overwrite:           bool = False,
) -> dict:
    """Convenience wrapper — SD textures only."""
    return run_ai_pipeline(
        buildings_json_path = buildings_json_path,
        output_dir          = output_dir,
        mode                = "textures",
        sd_backend          = backend,
        sd_api_key          = api_key,
        sd_model_id         = model_id,
        texture_size        = size,
        overwrite           = overwrite,
    )


def run_mesh_pipeline(
    buildings_json_path: str,
    output_dir:          str,
    resolution:          int = 256,
    device:              Optional[str] = None,
) -> dict:
    """Convenience wrapper — TripoSR meshes only (textures must already exist)."""
    return run_ai_pipeline(
        buildings_json_path = buildings_json_path,
        output_dir          = output_dir,
        mode                = "meshes",
        mesh_resolution     = resolution,
        device              = device,
    )


# ─── CLI ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse, sys, logging as _log

    sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
    from config import OUTPUT_DIR, BUILDING_DIR, GOOGLE_MAPS_API_KEY

    _log.basicConfig(
        level  = _log.INFO,
        format = "%(asctime)s  %(levelname)-7s  %(message)s",
        datefmt= "%H:%M:%S",
    )

    parser = argparse.ArgumentParser(description="Phase 4 AI Enhancement Pipeline")
    parser.add_argument(
        "--buildings", required=True,
        help="Path to *_buildings.json (from osm_fetcher)"
    )
    parser.add_argument(
        "--output-dir", default=os.path.join(OUTPUT_DIR, "ai"),
        help="Root output directory for textures/ and meshes/"
    )
    parser.add_argument(
        "--mode", default="textures",
        choices=["textures", "meshes", "full"],
        help="Which AI steps to run (default: textures)"
    )
    parser.add_argument("--sd-backend",      default="local", choices=["local", "api"])
    parser.add_argument("--sd-api-key",      default="")
    parser.add_argument("--sd-model-id",     default="")
    parser.add_argument("--texture-size",    type=int, default=512)
    parser.add_argument("--mesh-resolution", type=int, default=256)
    parser.add_argument("--device",          default=None)
    parser.add_argument("--overwrite",       action="store_true")

    args = parser.parse_args()

    run_ai_pipeline(
        buildings_json_path = args.buildings,
        output_dir          = args.output_dir,
        mode                = args.mode,
        sd_backend          = args.sd_backend,
        sd_api_key          = args.sd_api_key,
        sd_model_id         = args.sd_model_id,
        texture_size        = args.texture_size,
        mesh_resolution     = args.mesh_resolution,
        device              = args.device,
        overwrite           = args.overwrite,
    )
