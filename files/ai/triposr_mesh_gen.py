"""
TripoSR Mesh Generator
Phase 4 — AI Enhancement Pipeline

Generates 3D building meshes from reference images using TripoSR.

TripoSR:   https://github.com/VAST-AI-Research/TripoSR
HuggingFace: stabilityai/TripoSR

Requirements:
  pip install torch torchvision tsr trimesh pillow
  pip install rembg   (optional — better background removal)

Usage (standalone):
  # Single image → mesh
  python ai/triposr_mesh_gen.py image facade.png --output output/meshes/building.obj

  # Batch from SD texture manifest
  python ai/triposr_mesh_gen.py batch output/textures/texture_manifest.json \
      --output-dir output/meshes/
"""

import os
import json
import logging
import subprocess
import tempfile
from typing import Dict, List, Optional

log = logging.getLogger(__name__)


# ─── TripoSR Generator ────────────────────────────────────────────────────────

class TripoSRGenerator:
    """
    Wraps the TripoSR model for single-image → 3D mesh reconstruction.

    Input:  RGB/RGBA image of a building facade (512×512 recommended)
    Output: .obj mesh file
    """

    MODEL_ID = "stabilityai/TripoSR"

    def __init__(
        self,
        model_id:   Optional[str] = None,
        device:     Optional[str] = None,
        chunk_size: int = 8192,
    ):
        self.model_id   = model_id or self.MODEL_ID
        self.chunk_size = chunk_size
        self._model     = None
        self._device    = device

    def _load(self):
        if self._model is not None:
            return
        try:
            import torch
            from tsr.system import TSR  # noqa: F401
        except ImportError:
            raise ImportError(
                "TripoSR (tsr) package is required.\n"
                "  pip install torch torchvision\n"
                "  pip install git+https://github.com/VAST-AI-Research/TripoSR.git"
            )

        import torch
        from tsr.system import TSR

        device = self._device or ("cuda" if torch.cuda.is_available() else "cpu")
        log.info(f"Loading TripoSR '{self.model_id}' on {device} ...")

        model = TSR.from_pretrained(
            self.model_id,
            config_name = "config.yaml",
            weight_name = "model.ckpt",
        )
        model.renderer.set_chunk_size(self.chunk_size)
        model = model.to(device)

        self._model  = model
        self._device = device
        log.info("TripoSR ready.")

    def generate_mesh(
        self,
        image_path:    str,
        output_path:   str,
        mc_resolution: int  = 256,
        remove_bg:     bool = True,
    ) -> str:
        """
        Reconstruct a 3D mesh from a single image.

        Args:
            image_path:    Input PNG/JPG (RGB or RGBA)
            output_path:   Output .obj path
            mc_resolution: Marching Cubes resolution (128 fast / 256 balanced / 512 detail)
            remove_bg:     Auto-remove background for cleaner reconstruction

        Returns:
            Absolute path to the generated .obj file
        """
        self._load()

        import torch
        from PIL import Image

        img = Image.open(image_path).convert("RGB")

        if remove_bg:
            img = self._remove_background(img)

        img_tensor = self._preprocess(img)

        log.info(f"  TripoSR: {os.path.basename(image_path)} → {os.path.basename(output_path)}")

        with torch.no_grad():
            scene_codes = self._model([img_tensor], device=self._device)

        meshes = self._model.extract_mesh(scene_codes, resolution=mc_resolution)
        mesh   = meshes[0]

        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
        mesh.export(output_path)
        log.info(f"    Mesh saved: {output_path}")

        return os.path.abspath(output_path)

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _remove_background(self, img: "Image.Image") -> "Image.Image":
        """Remove image background via rembg (falls back gracefully if missing)."""
        try:
            import io
            from rembg import remove as rembg_remove
            from PIL import Image

            buf = io.BytesIO()
            img.save(buf, format="PNG")
            buf.seek(0)
            return Image.open(io.BytesIO(rembg_remove(buf.read()))).convert("RGBA")
        except ImportError:
            log.warning("rembg not installed — skipping background removal. pip install rembg")
            return img

    def _preprocess(self, img: "Image.Image", size: int = 512) -> "torch.Tensor":
        """Resize and convert image to a normalized tensor for TripoSR."""
        import torch
        import torchvision.transforms.functional as TF
        from PIL import Image

        if img.mode == "RGBA":
            bg = Image.new("RGB", img.size, (255, 255, 255))
            bg.paste(img, mask=img.split()[3])
            img = bg

        img    = img.resize((size, size), Image.LANCZOS)
        tensor = TF.to_tensor(img)          # [3, H, W] in [0, 1]
        return tensor


# ─── Batch Pipeline (from SD texture manifest) ───────────────────────────────

def generate_building_meshes(
    texture_manifest_path: str,
    output_dir:            str,
    generator:             Optional[TripoSRGenerator] = None,
    mc_resolution:         int  = 256,
    surfaces:              Optional[List[str]] = None,
) -> Dict[str, str]:
    """
    For each building type, use its wall albedo as TripoSR input to generate a mesh.

    Args:
        texture_manifest_path: texture_manifest.json from sd_texture_gen
        output_dir:            Where .obj files are saved
        generator:             TripoSRGenerator (created automatically if None)
        mc_resolution:         Marching Cubes resolution
        surfaces:              Surface images to use (default: ["wall"])

    Returns:
        {building_type: obj_path, ...}
    """
    if surfaces is None:
        surfaces = ["wall"]

    with open(texture_manifest_path) as f:
        manifest = json.load(f)

    if generator is None:
        generator = TripoSRGenerator()

    os.makedirs(output_dir, exist_ok=True)
    results: Dict[str, str] = {}

    log.info(
        f"TripoSR batch: {len(manifest)} types  |  "
        f"resolution={mc_resolution}  |  output={output_dir}"
    )

    for btype, surface_map in manifest.items():
        for surface in surfaces:
            if surface not in surface_map:
                continue

            albedo_path = surface_map[surface].get("albedo", "")
            if not albedo_path or not os.path.exists(albedo_path):
                log.warning(f"  Albedo missing for {btype}/{surface} — skipping")
                continue

            out_path = os.path.join(output_dir, f"{btype}_{surface}.obj")
            if os.path.exists(out_path):
                log.info(f"  Already exists, skipping: {os.path.basename(out_path)}")
                results[btype] = out_path
                continue

            try:
                mesh_path = generator.generate_mesh(
                    image_path    = albedo_path,
                    output_path   = out_path,
                    mc_resolution = mc_resolution,
                )
                results[btype] = mesh_path
            except Exception as e:
                log.error(f"  Failed {btype}: {e}")

    manifest_path = os.path.join(output_dir, "mesh_manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(results, f, indent=2)
    log.info(f"Mesh manifest: {manifest_path}")

    return results


# ─── Blender Facade Render (fallback when no SD textures) ────────────────────

def render_facade_from_fbx(
    fbx_path:   str,
    output_path: str,
    resolution:  int = 512,
    blender_exe: str = "blender",
) -> Optional[str]:
    """
    Render a front-facing image of a Blender FBX mesh.
    Used as TripoSR input when SD textures are not available.

    Returns path to rendered PNG, or None on failure.
    """
    script = f"""
import bpy, math
from mathutils import Vector

bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete()

bpy.ops.import_scene.fbx(filepath=r"{fbx_path}")
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
if not objs:
    raise RuntimeError("No mesh objects found")

min_co = [float('inf')] * 3
max_co = [float('-inf')] * 3
for obj in objs:
    for v in obj.bound_box:
        wv = obj.matrix_world @ Vector(v)
        for i in range(3):
            min_co[i] = min(min_co[i], wv[i])
            max_co[i] = max(max_co[i], wv[i])

cx = (min_co[0] + max_co[0]) / 2
cy = (min_co[1] + max_co[1]) / 2
cz = (min_co[2] + max_co[2]) / 2
size = max(max_co[i] - min_co[i] for i in range(3))

cam_data = bpy.data.cameras.new("Cam")
cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.scene.collection.objects.link(cam)
bpy.context.scene.camera = cam

cam.location       = (cx, cy - size * 2, cz)
cam.rotation_euler = (math.pi / 2, 0, 0)

bpy.context.scene.render.resolution_x = {resolution}
bpy.context.scene.render.resolution_y = {resolution}
bpy.context.scene.render.filepath = r"{output_path}"
bpy.context.scene.render.image_settings.file_format = 'PNG'
bpy.ops.render.render(write_still=True)
print("RENDER_OK")
"""

    with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as tmp:
        tmp.write(script)
        tmp_path = tmp.name

    try:
        result = subprocess.run(
            [blender_exe, "--background", "--python", tmp_path],
            capture_output=True, text=True, timeout=120,
        )
        if "RENDER_OK" in result.stdout and os.path.exists(output_path):
            log.info(f"Blender render OK: {output_path}")
            return output_path
        log.error(f"Blender render failed:\n{result.stderr[-500:]}")
        return None
    except Exception as e:
        log.error(f"Blender render exception: {e}")
        return None
    finally:
        os.unlink(tmp_path)


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

    parser = argparse.ArgumentParser(description="TripoSR Building Mesh Generator")
    subs   = parser.add_subparsers(dest="cmd", required=True)

    # Single image
    p_img = subs.add_parser("image", help="Generate mesh from a single image")
    p_img.add_argument("image",            help="Input image path")
    p_img.add_argument("--output",         required=True, help="Output .obj path")
    p_img.add_argument("--resolution",     type=int, default=256)
    p_img.add_argument("--no-remove-bg",   action="store_true")
    p_img.add_argument("--device",         default=None)

    # Batch from texture manifest
    p_bat = subs.add_parser("batch", help="Generate meshes for all building types")
    p_bat.add_argument("manifest",         help="Path to texture_manifest.json")
    p_bat.add_argument("--output-dir",
                       default=os.path.join(OUTPUT_DIR, "meshes"))
    p_bat.add_argument("--resolution",     type=int, default=256)
    p_bat.add_argument("--device",         default=None)

    args = parser.parse_args()

    if args.cmd == "image":
        gen = TripoSRGenerator(device=args.device)
        gen.generate_mesh(
            image_path    = args.image,
            output_path   = args.output,
            mc_resolution = args.resolution,
            remove_bg     = not args.no_remove_bg,
        )

    elif args.cmd == "batch":
        gen = TripoSRGenerator(device=args.device)
        generate_building_meshes(
            texture_manifest_path = args.manifest,
            output_dir            = args.output_dir,
            generator             = gen,
            mc_resolution         = args.resolution,
        )
