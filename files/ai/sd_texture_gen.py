"""
Stable Diffusion PBR Texture Generator
Phase 4 — AI Enhancement Pipeline

Generates albedo, normal, and roughness textures per building type.

Requirements (local backend):
  pip install diffusers transformers accelerate torch pillow scipy

Requirements (api backend):
  pip install requests pillow
  Set env var: STABILITY_API_KEY

Usage (standalone):
  python ai/sd_texture_gen.py --backend local --size 512
  python ai/sd_texture_gen.py --backend api --api-key YOUR_KEY --size 1024
"""

import os
import json
import logging
from typing import Dict, List, Optional

import numpy as np
from PIL import Image

log = logging.getLogger(__name__)


# ─── Prompts Per Building Type ────────────────────────────────────────────────

TEXTURE_PROMPTS: Dict[str, Dict[str, str]] = {
    "BP_Building_Residential": {
        "wall": (
            "brick wall texture, residential house, aged weathered red bricks, "
            "mortar joints, seamless tileable, photorealistic, 4k"
        ),
        "roof": (
            "clay roof tiles, terracotta, slightly weathered, seamless tileable, "
            "photorealistic, top-down view"
        ),
        "neg": "cartoon, illustration, low quality, blurry, watermark",
    },
    "BP_Building_Apartment": {
        "wall": (
            "concrete apartment building facade, grey concrete panels, "
            "urban, slightly dirty, seamless tileable, photorealistic, 4k"
        ),
        "roof": (
            "flat concrete roof surface, pebble gravel, seamless tileable, photorealistic"
        ),
        "neg": "cartoon, illustration, low quality",
    },
    "BP_Building_Commercial": {
        "wall": (
            "glass and steel commercial building facade, reflective glass panels, "
            "modern architecture, seamless tileable, photorealistic"
        ),
        "roof": (
            "flat commercial roof, HVAC units, gravel surface, "
            "seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Industrial": {
        "wall": (
            "corrugated metal sheet wall, industrial warehouse, "
            "rusty weathered metal, paint chipping, seamless tileable, photorealistic"
        ),
        "roof": (
            "corrugated metal roof, rust stains, industrial, "
            "seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Office": {
        "wall": (
            "office building exterior, glass curtain wall, aluminium frame, "
            "modern architecture, seamless tileable, photorealistic, 4k"
        ),
        "roof": (
            "flat office building roof, membrane roofing, seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Retail": {
        "wall": (
            "retail shop facade, painted plaster wall, colorful storefront, "
            "slightly weathered, seamless tileable, photorealistic"
        ),
        "roof": (
            "flat retail awning surface, canvas texture, seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Warehouse": {
        "wall": (
            "warehouse exterior wall, large corrugated metal panels, industrial, "
            "faded paint, seamless tileable, photorealistic"
        ),
        "roof": (
            "warehouse metal roof, corrugated, industrial, "
            "seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Church": {
        "wall": (
            "stone church wall texture, Gothic architecture, aged limestone blocks, "
            "moss patches, seamless tileable, photorealistic"
        ),
        "roof": (
            "slate church roof tiles, grey, aged, mossy, "
            "seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_School": {
        "wall": (
            "school building brick facade, red brick, institutional, "
            "clean mortar joints, seamless tileable, photorealistic"
        ),
        "roof": (
            "flat school roof, asphalt membrane, seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Hospital": {
        "wall": (
            "hospital building exterior, clean white plaster wall, "
            "clinical, smooth render, seamless tileable, photorealistic"
        ),
        "roof": (
            "hospital flat roof, white membrane, clean, "
            "seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
    "BP_Building_Generic": {
        "wall": (
            "generic urban building wall, concrete plaster, slightly weathered, "
            "seamless tileable, photorealistic"
        ),
        "roof": (
            "generic flat roof, concrete screed, seamless tileable, photorealistic"
        ),
        "neg": "cartoon, low quality",
    },
}


# ─── PBR Map Derivation ───────────────────────────────────────────────────────

def albedo_to_normal(albedo: Image.Image, strength: float = 2.0) -> Image.Image:
    """Derive a normal map from albedo via Sobel gradients."""
    from scipy.ndimage import sobel

    gray = np.array(albedo.convert("L")).astype(np.float32) / 255.0
    dx = sobel(gray, axis=1) * strength
    dy = sobel(gray, axis=0) * strength

    normal = np.stack([-dx, -dy, np.ones_like(dx)], axis=-1)
    norm   = np.linalg.norm(normal, axis=-1, keepdims=True)
    normal = normal / (norm + 1e-8)

    normal_img = ((normal * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)
    return Image.fromarray(normal_img, "RGB")


def albedo_to_roughness(albedo: Image.Image) -> Image.Image:
    """Derive a roughness map from albedo. Textured/dark areas = rough."""
    from scipy.ndimage import uniform_filter

    gray     = np.array(albedo.convert("L")).astype(np.float32) / 255.0
    mean     = uniform_filter(gray, size=5)
    sq_mean  = uniform_filter(gray ** 2, size=5)
    variance = np.clip(sq_mean - mean ** 2, 0, None)

    roughness = 1.0 - (gray * 0.5) + (np.sqrt(variance) * 2.0)
    roughness = np.clip(roughness, 0, 1)

    return Image.fromarray((roughness * 255).astype(np.uint8), "L")


# ─── Seamless Tile Helper ─────────────────────────────────────────────────────

def make_seamless(img: Image.Image, blend_width: int = 64) -> Image.Image:
    """Blend opposite edges so the texture tiles without seams."""
    arr = np.array(img).astype(np.float32)
    h, w = arr.shape[:2]
    bw = min(blend_width, w // 4, h // 4)
    out = arr.copy()

    for i in range(bw):
        alpha = i / bw
        out[:, i]        = arr[:, i] * alpha       + arr[:, w - bw + i] * (1 - alpha)
        out[:, w - bw + i] = arr[:, w - bw + i] * alpha + arr[:, i]       * (1 - alpha)

    for i in range(bw):
        alpha = i / bw
        out[i, :]        = out[i, :] * alpha       + out[h - bw + i, :] * (1 - alpha)
        out[h - bw + i, :] = out[h - bw + i, :] * alpha + out[i, :]       * (1 - alpha)

    return Image.fromarray(out.clip(0, 255).astype(np.uint8))


# ─── Local SD Backend ─────────────────────────────────────────────────────────

class LocalSDBackend:
    """Generates textures using a local Stable Diffusion model via diffusers."""

    DEFAULT_MODEL = "stabilityai/stable-diffusion-2-1"

    def __init__(self, model_id: Optional[str] = None, device: Optional[str] = None):
        self.model_id = model_id or self.DEFAULT_MODEL
        self._pipe    = None
        self._device  = device

    def _load(self):
        if self._pipe is not None:
            return
        try:
            import torch
            from diffusers import StableDiffusionPipeline, DPMSolverMultistepScheduler
        except ImportError:
            raise ImportError(
                "diffusers and torch are required for the local backend.\n"
                "  pip install diffusers transformers accelerate torch"
            )

        import torch
        from diffusers import StableDiffusionPipeline, DPMSolverMultistepScheduler

        device = self._device or ("cuda" if torch.cuda.is_available() else "cpu")
        log.info(f"Loading SD model '{self.model_id}' on {device} ...")

        pipe = StableDiffusionPipeline.from_pretrained(
            self.model_id,
            torch_dtype  = torch.float16 if device == "cuda" else torch.float32,
            safety_checker = None,
        )
        pipe.scheduler = DPMSolverMultistepScheduler.from_config(pipe.scheduler.config)
        pipe = pipe.to(device)
        pipe.enable_attention_slicing()

        self._pipe   = pipe
        self._device = device
        log.info("SD model ready.")

    def generate(
        self,
        prompt:     str,
        neg_prompt: str  = "",
        size:       int  = 512,
        steps:      int  = 25,
        guidance:   float = 7.5,
        seed:       Optional[int] = None,
    ) -> Image.Image:
        self._load()
        import torch
        gen = torch.Generator(device=self._pipe.device)
        if seed is not None:
            gen.manual_seed(seed)
        result = self._pipe(
            prompt,
            negative_prompt    = neg_prompt,
            width              = size,
            height             = size,
            num_inference_steps = steps,
            guidance_scale     = guidance,
            generator          = gen,
        )
        return result.images[0]


# ─── Stability AI REST API Backend ───────────────────────────────────────────

class StabilityAPIBackend:
    """Generates textures via Stability AI REST API (SDXL)."""

    API_URL = (
        "https://api.stability.ai/v1/generation"
        "/stable-diffusion-xl-1024-v1-0/text-to-image"
    )

    def __init__(self, api_key: Optional[str] = None):
        self.api_key = api_key or os.environ.get("STABILITY_API_KEY", "")
        if not self.api_key:
            raise ValueError(
                "Stability API key required.\n"
                "  Set env var STABILITY_API_KEY or pass api_key=."
            )

    def generate(
        self,
        prompt:     str,
        neg_prompt: str  = "",
        size:       int  = 1024,
        steps:      int  = 30,
        guidance:   float = 7.0,
        seed:       Optional[int] = None,
    ) -> Image.Image:
        import requests, io, base64

        payload: dict = {
            "text_prompts": [
                {"text": prompt,     "weight":  1.0},
                {"text": neg_prompt, "weight": -1.0},
            ],
            "width":     size,
            "height":    size,
            "steps":     steps,
            "cfg_scale": guidance,
            "samples":   1,
        }
        if seed is not None:
            payload["seed"] = seed

        resp = requests.post(
            self.API_URL,
            headers = {
                "Authorization": f"Bearer {self.api_key}",
                "Accept":        "application/json",
            },
            json    = payload,
            timeout = 120,
        )
        resp.raise_for_status()
        img_b64 = resp.json()["artifacts"][0]["base64"]
        return Image.open(io.BytesIO(base64.b64decode(img_b64)))


# ─── Per-Type Texture Set Generator ──────────────────────────────────────────

def generate_texture_set(
    building_type: str,
    output_dir:    str,
    backend,
    surface:       str = "wall",
    size:          int = 512,
    seed:          Optional[int] = None,
    overwrite:     bool = False,
) -> Dict[str, str]:
    """
    Generate albedo + normal + roughness for one building type + surface.

    Returns:
        {"albedo": path, "normal": path, "roughness": path}
    """
    os.makedirs(output_dir, exist_ok=True)

    prompts   = TEXTURE_PROMPTS.get(building_type, TEXTURE_PROMPTS["BP_Building_Generic"])
    prompt    = prompts.get(surface, prompts["wall"])
    neg       = prompts.get("neg", "cartoon, low quality")

    prefix = f"{building_type}_{surface}"
    paths  = {
        "albedo":    os.path.join(output_dir, f"{prefix}_albedo.png"),
        "normal":    os.path.join(output_dir, f"{prefix}_normal.png"),
        "roughness": os.path.join(output_dir, f"{prefix}_roughness.png"),
    }

    if not overwrite and all(os.path.exists(p) for p in paths.values()):
        log.info(f"  Skipping (already exists): {prefix}")
        return paths

    log.info(f"  Generating: {prefix}")
    log.debug(f"    Prompt: {prompt[:80]}...")

    albedo = backend.generate(prompt, neg_prompt=neg, size=size, seed=seed)
    albedo = make_seamless(albedo)
    albedo.save(paths["albedo"])

    normal = albedo_to_normal(albedo)
    normal.save(paths["normal"])

    roughness = albedo_to_roughness(albedo)
    roughness.save(paths["roughness"])

    log.info(f"    Saved: albedo / normal / roughness")
    return paths


# ─── Batch Generator ─────────────────────────────────────────────────────────

def generate_all_textures(
    output_dir:   str,
    backend_type: str = "local",
    api_key:      str = "",
    model_id:     str = "",
    size:         int = 512,
    surfaces:     Optional[List[str]] = None,
    overwrite:    bool = False,
) -> Dict[str, Dict[str, Dict[str, str]]]:
    """
    Generate PBR texture sets for all 11 building types.

    Returns:
        {
          "BP_Building_Residential": {
            "wall": {"albedo": "...", "normal": "...", "roughness": "..."},
            "roof": {...},
          },
          ...
        }
    """
    if surfaces is None:
        surfaces = ["wall", "roof"]

    if backend_type == "api":
        backend = StabilityAPIBackend(api_key=api_key)
    else:
        backend = LocalSDBackend(model_id=model_id or None)

    results        = {}
    building_types = list(TEXTURE_PROMPTS.keys())
    total          = len(building_types) * len(surfaces)

    log.info(
        f"SD Texture Generation: {len(building_types)} types × {len(surfaces)} surfaces "
        f"= {total} sets  |  backend={backend_type}  size={size}"
    )

    n = 0
    for btype in building_types:
        results[btype] = {}
        for surface in surfaces:
            n += 1
            log.info(f"[{n}/{total}] {btype} / {surface}")
            paths = generate_texture_set(
                building_type = btype,
                output_dir    = output_dir,
                backend       = backend,
                surface       = surface,
                size          = size,
                seed          = hash(btype + surface) % (2 ** 32),
                overwrite     = overwrite,
            )
            results[btype][surface] = paths

    manifest_path = os.path.join(output_dir, "texture_manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(results, f, indent=2)
    log.info(f"\nTexture manifest: {manifest_path}")

    return results


# ─── CLI ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse, sys, logging as _log

    sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
    from config import OUTPUT_DIR

    _log.basicConfig(
        level  = _log.INFO,
        format = "%(asctime)s  %(levelname)-7s  %(message)s",
        datefmt= "%H:%M:%S",
    )

    parser = argparse.ArgumentParser(description="SD PBR Texture Generator")
    parser.add_argument("--output-dir",  default=os.path.join(OUTPUT_DIR, "textures"))
    parser.add_argument("--backend",     default="local", choices=["local", "api"])
    parser.add_argument("--api-key",     default="")
    parser.add_argument("--model-id",    default="")
    parser.add_argument("--size",        type=int, default=512)
    parser.add_argument("--surfaces",    nargs="+", default=["wall", "roof"])
    parser.add_argument("--overwrite",   action="store_true")
    args = parser.parse_args()

    generate_all_textures(
        output_dir   = args.output_dir,
        backend_type = args.backend,
        api_key      = args.api_key,
        model_id     = args.model_id,
        size         = args.size,
        surfaces     = args.surfaces,
        overwrite    = args.overwrite,
    )
