"""
Level Tool Configuration
Extraction Shooter Map Generation for UE5
"""

# ─── API Keys (fill in before use) ────────────────────────────────────────────
GOOGLE_MAPS_API_KEY = ""          # Google Elevation + 3D Tiles
MAPBOX_ACCESS_TOKEN = ""          # Mapbox Terrain-RGB (optional)

# ─── UE5 Scale ─────────────────────────────────────────────────────────────────
# 1 UE Unit = 1 cm  →  100 UU = 1 m
UE_UNIT_PER_METER    = 100.0

# Landscape Z scale: 1.0 = 256 meters full height range
# Set so heightmap 0-65535 maps to this real-world meter range
LANDSCAPE_Z_RANGE_METERS = 400.0    # -200m ~ +200m

# Landscape XY scale (cm per quad)
LANDSCAPE_XY_SCALE   = 100.0        # 1 m per landscape quad

# ─── Heightmap Sizes (UE5 valid: 2^n+1 per side) ────────────────────────────
# 505, 1009, 2017, 4033, 8129
HEIGHTMAP_SIZE = 1009               # 1009×1009 → ~1km × 1km at 1m/quad

# ─── Coordinate Presets ──────────────────────────────────────────────────────
# Ruined city extraction shooter map candidates
COORD_PRESETS = {
    "Seoul_Jongno":   {"lat": 37.5704, "lon": 126.9820, "radius_km": 1.0},
    "SantaXLAR":      {"lat": 26.370425, "lon": 56.359094, "radius_km": 1.0},
    "Chernobyl":      {"lat": 51.3890, "lon": 30.0993,  "radius_km": 1.5},
    "Detroit_Downtown":{"lat": 42.3314,"lon": -83.0457,  "radius_km": 1.2},
    "Pripyat":        {"lat": 51.4072, "lon": 30.0566,  "radius_km": 1.0},
    "Incheon_Port":   {"lat": 37.4536, "lon": 126.7020, "radius_km": 1.0},
}

# ─── OSM Building Query Settings ─────────────────────────────────────────────
OVERPASS_URL   = "https://overpass-api.de/api/interpreter"
DEFAULT_BUILDING_HEIGHT_M = 10.0    # fallback if no height tag in OSM
MIN_BUILDING_AREA_M2      = 20.0    # ignore tiny structures

# Building type → UE5 mesh pool key
BUILDING_TYPE_MAP = {
    "residential":  "BP_Building_Residential",
    "apartments":   "BP_Building_Apartment",
    "commercial":   "BP_Building_Commercial",
    "industrial":   "BP_Building_Industrial",
    "office":       "BP_Building_Office",
    "retail":       "BP_Building_Retail",
    "warehouse":    "BP_Building_Warehouse",
    "church":       "BP_Building_Church",
    "school":       "BP_Building_School",
    "hospital":     "BP_Building_Hospital",
    "yes":          "BP_Building_Generic",     # untagged
}

# ─── Output Paths ────────────────────────────────────────────────────────────
import os
BASE_DIR      = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR    = os.path.join(BASE_DIR, "output")
HEIGHTMAP_DIR = os.path.join(OUTPUT_DIR, "heightmaps")
BUILDING_DIR  = os.path.join(OUTPUT_DIR, "buildings")
UE5_DIR       = os.path.join(OUTPUT_DIR, "ue5_scripts")
WATER_DIR     = os.path.join(OUTPUT_DIR, "water")

for d in [OUTPUT_DIR, HEIGHTMAP_DIR, BUILDING_DIR, UE5_DIR, WATER_DIR]:
    os.makedirs(d, exist_ok=True)
