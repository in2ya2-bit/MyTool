"""
Erangel Reference Map Generation — Expected Output Visualization
Generates a visual mockup based on LevelTool_ReferenceMapGen design doc POI data.
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, Circle
import numpy as np
from matplotlib.colors import LinearSegmentedColormap

MAP_SIZE_KM = 8
MAP_SIZE_PX = 800

POIS = [
    {"name": "Sosnovka\nMilitary Base", "cx": 0.52, "cy": 0.85, "r": 500, "style": "military", "tier": "S", "buildings": 30, "island": True},
    {"name": "Pochinki",     "cx": 0.42, "cy": 0.52, "r": 300, "style": "eastern_rural", "tier": "S", "buildings": 42},
    {"name": "School",       "cx": 0.47, "cy": 0.38, "r": 100, "style": "institutional", "tier": "S", "buildings": 5},
    {"name": "Georgopol",    "cx": 0.18, "cy": 0.35, "r": 400, "style": "port_industrial", "tier": "A", "buildings": 80},
    {"name": "Yasnaya\nPolyana", "cx": 0.72, "cy": 0.28, "r": 350, "style": "dense_urban", "tier": "A", "buildings": 60},
    {"name": "Mylta Power",  "cx": 0.85, "cy": 0.65, "r": 150, "style": "power_plant", "tier": "A", "buildings": 15},
    {"name": "Novorepnoye",  "cx": 0.88, "cy": 0.82, "r": 200, "style": "port_industrial", "tier": "A", "buildings": 35},
    {"name": "Rozhok",       "cx": 0.45, "cy": 0.35, "r": 200, "style": "eastern_rural", "tier": "B", "buildings": 25},
    {"name": "Prison",       "cx": 0.65, "cy": 0.60, "r": 120, "style": "institutional", "tier": "B", "buildings": 8},
    {"name": "Mansion",      "cx": 0.72, "cy": 0.52, "r": 80, "style": "eastern_rural", "tier": "B", "buildings": 3},
    {"name": "Primorsk",     "cx": 0.18, "cy": 0.78, "r": 200, "style": "eastern_rural", "tier": "B", "buildings": 20},
    {"name": "Ferry Pier",   "cx": 0.35, "cy": 0.82, "r": 100, "style": "port_industrial", "tier": "B", "buildings": 8},
    {"name": "Hospital",     "cx": 0.13, "cy": 0.48, "r": 100, "style": "institutional", "tier": "B", "buildings": 4},
    {"name": "Shooting\nRange", "cx": 0.35, "cy": 0.15, "r": 100, "style": "military", "tier": "B", "buildings": 8},
    {"name": "Stalber",      "cx": 0.72, "cy": 0.12, "r": 80, "style": "military_outpost", "tier": "C", "buildings": 5},
    {"name": "Zharki",       "cx": 0.08, "cy": 0.08, "r": 150, "style": "eastern_rural", "tier": "C", "buildings": 12},
    {"name": "Severny",      "cx": 0.30, "cy": 0.08, "r": 150, "style": "eastern_rural", "tier": "C", "buildings": 15},
    {"name": "Gatka",        "cx": 0.25, "cy": 0.55, "r": 150, "style": "eastern_rural", "tier": "C", "buildings": 12},
    {"name": "Quarry",       "cx": 0.18, "cy": 0.68, "r": 120, "style": "quarry", "tier": "C", "buildings": 5},
    {"name": "Lipovka",      "cx": 0.80, "cy": 0.42, "r": 150, "style": "eastern_rural", "tier": "C", "buildings": 15},
    {"name": "Kameshki",     "cx": 0.92, "cy": 0.10, "r": 120, "style": "eastern_rural", "tier": "C", "buildings": 10},
]

CONNECTIONS = [
    ("Pochinki", "Rozhok"), ("Rozhok", "School"), ("School", "Pochinki"),
    ("Rozhok", "Georgopol"), ("Georgopol", "Zharki"), ("Zharki", "Severny"),
    ("Severny", "Shooting\nRange"), ("Shooting\nRange", "Stalber"),
    ("Stalber", "Kameshki"), ("Yasnaya\nPolyana", "Lipovka"),
    ("Lipovka", "Prison"), ("Prison", "Mansion"),
    ("Pochinki", "Gatka"), ("Gatka", "Hospital"), ("Hospital", "Georgopol"),
    ("Quarry", "Primorsk"), ("Primorsk", "Ferry Pier"),
    ("Mylta Power", "Novorepnoye"),
    ("Yasnaya\nPolyana", "Stalber"),
]

TIER_COLORS = {"S": "#FF3333", "A": "#FF9933", "B": "#3399FF", "C": "#88AA88"}
TIER_SIZES  = {"S": 14, "A": 11, "B": 9, "C": 7}

STYLE_FILL = {
    "military": "#556B2F", "military_outpost": "#6B8E23",
    "eastern_rural": "#D2B48C", "port_industrial": "#778899",
    "dense_urban": "#A0522D", "institutional": "#BC8F8F",
    "power_plant": "#808080", "quarry": "#C4A882",
}


def make_island_shape():
    """Approximate Erangel main island + military island as polygon vertices."""
    t = np.linspace(0, 2*np.pi, 120)

    # Main island (irregular blob, roughly centered at 0.42, 0.38)
    main_r = 0.34 + 0.06*np.sin(3*t) + 0.04*np.cos(5*t) + 0.03*np.sin(7*t)
    main_x = 0.44 + main_r * np.cos(t) * 1.15
    main_y = 0.38 + main_r * np.sin(t) * 1.05

    main_x = np.clip(main_x, 0.02, 0.98)
    main_y = np.clip(main_y, 0.02, 0.72)

    # Military island (smaller, bottom)
    t2 = np.linspace(0, 2*np.pi, 60)
    mil_r = 0.12 + 0.02*np.sin(4*t2)
    mil_x = 0.52 + mil_r * np.cos(t2) * 1.3
    mil_y = 0.84 + mil_r * np.sin(t2) * 0.6

    return (main_x, main_y), (mil_x, mil_y)


def generate_heightmap(shape_x, shape_y):
    """Generate a simple heightmap grid."""
    from matplotlib.path import Path
    grid_size = 200
    x = np.linspace(0, 1, grid_size)
    y = np.linspace(0, 1, grid_size)
    X, Y = np.meshgrid(x, y)
    hmap = np.full((grid_size, grid_size), -1.0)

    path = Path(np.column_stack([shape_x, shape_y]))
    points = np.column_stack([X.ravel(), Y.ravel()])
    mask = path.contains_points(points).reshape(grid_size, grid_size)

    for i in range(grid_size):
        for j in range(grid_size):
            if mask[i, j]:
                cx, cy = X[i, j], Y[i, j]
                dist_coast = 0.05
                for k in range(len(shape_x)):
                    d = np.sqrt((shape_x[k]-cx)**2 + (shape_y[k]-cy)**2)
                    dist_coast = min(dist_coast, d) if d < dist_coast else dist_coast
                base = dist_coast * 600
                base += 30 * np.sin(cx*8) * np.cos(cy*6)
                hmap[i, j] = np.clip(base, 0, 200)

    return X, Y, hmap, mask


def draw_buildings_in_poi(ax, poi, count=None):
    """Scatter small rectangles inside POI radius to represent buildings."""
    np.random.seed(hash(poi["name"]) % 2**31)
    n = count or min(poi["buildings"], 30)
    cx, cy = poi["cx"], poi["cy"]
    r_norm = poi["r"] / (MAP_SIZE_KM * 1000)

    fill = STYLE_FILL.get(poi["style"], "#AA9977")

    for _ in range(n):
        angle = np.random.uniform(0, 2*np.pi)
        dist = np.random.uniform(0.15, 0.9) * r_norm
        bx = cx + dist * np.cos(angle)
        by = cy + dist * np.sin(angle)
        size = np.random.uniform(0.003, 0.008)
        rect = plt.Rectangle((bx - size/2, by - size/2), size, size * np.random.uniform(0.6, 1.4),
                              color=fill, alpha=0.7, linewidth=0.3, edgecolor='#333')
        ax.add_patch(rect)


def main():
    fig = plt.figure(figsize=(20, 10), facecolor='#1a1a2e')

    # ═══════════════════════════════════════════════════════
    # LEFT: Pipeline concept diagram
    # ═══════════════════════════════════════════════════════
    ax_left = fig.add_axes([0.02, 0.05, 0.28, 0.90])
    ax_left.set_xlim(0, 10)
    ax_left.set_ylim(0, 16)
    ax_left.set_facecolor('#1a1a2e')
    ax_left.axis('off')

    ax_left.text(5, 15.3, 'Generation Pipeline', ha='center', fontsize=14,
                 fontweight='bold', color='white', family='monospace')

    boxes = [
        (15.0, '#2d6a4f', 'INPUT',           'map_clean.png\npois.json\nbuilding_styles.json'),
        (12.5, '#1b4332', 'STEP 1',          'Image Analysis\nHSV → Land/Sea Mask\nBrightness → Heightmap\nThreshold → Roads'),
        (10.0, '#264653', 'STEP 2',          'Metadata Merge\nPOI → World Coords\nElevation Correction\nRoad Validation'),
        (7.5,  '#4a4e69', 'STEP 3',          'UE5 Generation\nLandscape + Water\nRoads + Buildings'),
        (5.0,  '#9b2226', 'OUTPUT',          'Erangel-like Map\n~70-80% similarity'),
    ]

    for y, color, label, desc in boxes:
        box = FancyBboxPatch((1, y-0.9), 8, 1.8, boxstyle="round,pad=0.2",
                             facecolor=color, edgecolor='white', linewidth=1.5, alpha=0.85)
        ax_left.add_patch(box)
        ax_left.text(2.0, y+0.3, label, fontsize=9, fontweight='bold', color='#FFD700', family='monospace')
        ax_left.text(2.0, y-0.3, desc, fontsize=7, color='white', family='monospace', linespacing=1.4)

    for i in range(len(boxes)-1):
        y1 = boxes[i][0] - 0.9
        y2 = boxes[i+1][0] + 0.9
        ax_left.annotate('', xy=(5, y2), xytext=(5, y1),
                         arrowprops=dict(arrowstyle='->', color='white', lw=2))

    # ═══════════════════════════════════════════════════════
    # RIGHT: Expected generated map
    # ═══════════════════════════════════════════════════════
    ax = fig.add_axes([0.32, 0.05, 0.66, 0.90])
    ax.set_xlim(-0.02, 1.02)
    ax.set_ylim(1.02, -0.02)
    ax.set_facecolor('#2B4865')
    ax.set_aspect('equal')

    ax.text(0.5, -0.01, 'Expected Output: Erangel (8×8km)', ha='center', fontsize=14,
            fontweight='bold', color='white', family='monospace',
            transform=ax.transData)

    (mx, my), (milx, mily) = make_island_shape()

    water_cmap = LinearSegmentedColormap.from_list('water', ['#1B3A4B', '#2B4865', '#3B5A7A'])
    for i in range(3):
        s = 1.0 + i * 0.01
        ax.fill(mx*s + (1-s)*0.5, my*s + (1-s)*0.5, color='#2B4865', alpha=0.3)

    land_cmap = LinearSegmentedColormap.from_list('land', ['#3B5E3B', '#5A7D4A', '#8BAA6B', '#A8C686'])

    ax.fill(mx, my, color='#5A7D4A', alpha=0.95, edgecolor='#3B5E3B', linewidth=1.5)
    ax.fill(milx, mily, color='#4A6D3A', alpha=0.95, edgecolor='#3B5E3B', linewidth=1.5)

    # Bridges
    ax.plot([0.42, 0.42], [0.71, 0.79], color='#AA9966', linewidth=3, solid_capstyle='round')
    ax.plot([0.58, 0.60], [0.71, 0.79], color='#AA9966', linewidth=3, solid_capstyle='round')
    ax.text(0.40, 0.75, 'Bridge', fontsize=5, color='#DDD', rotation=90, ha='center')
    ax.text(0.61, 0.75, 'Bridge', fontsize=5, color='#DDD', rotation=90, ha='center')

    # Roads
    poi_map = {p["name"]: p for p in POIS}
    for a_name, b_name in CONNECTIONS:
        if a_name in poi_map and b_name in poi_map:
            a, b = poi_map[a_name], poi_map[b_name]
            mid_x = (a["cx"] + b["cx"]) / 2 + np.random.uniform(-0.02, 0.02)
            mid_y = (a["cy"] + b["cy"]) / 2 + np.random.uniform(-0.02, 0.02)
            ax.plot([a["cx"], mid_x, b["cx"]], [a["cy"], mid_y, b["cy"]],
                    color='#C4A860', linewidth=1.0, alpha=0.6, linestyle='-')

    # Buildings inside POIs
    for poi in POIS:
        draw_buildings_in_poi(ax, poi)

    # POI circles and labels
    for poi in POIS:
        r_norm = poi["r"] / (MAP_SIZE_KM * 1000)
        tier_color = TIER_COLORS[poi["tier"]]

        circle = plt.Circle((poi["cx"], poi["cy"]), r_norm,
                             fill=False, edgecolor=tier_color, linewidth=1.2,
                             linestyle='--', alpha=0.7)
        ax.add_patch(circle)

        fontsize = TIER_SIZES[poi["tier"]] - 3
        ax.text(poi["cx"], poi["cy"] - r_norm - 0.015,
                poi["name"], ha='center', va='top',
                fontsize=fontsize, color='white', fontweight='bold',
                family='monospace',
                bbox=dict(boxstyle='round,pad=0.15', facecolor=tier_color, alpha=0.6, edgecolor='none'))

    # Legend
    legend_items = [
        mpatches.Patch(facecolor=TIER_COLORS["S"], label="S-Tier POI", alpha=0.8),
        mpatches.Patch(facecolor=TIER_COLORS["A"], label="A-Tier POI", alpha=0.8),
        mpatches.Patch(facecolor=TIER_COLORS["B"], label="B-Tier POI", alpha=0.8),
        mpatches.Patch(facecolor=TIER_COLORS["C"], label="C-Tier POI", alpha=0.8),
        mpatches.Patch(facecolor='#C4A860', label="Roads", alpha=0.8),
        mpatches.Patch(facecolor='#AA9966', label="Bridges", alpha=0.8),
        mpatches.Patch(facecolor='#5A7D4A', label="Land", alpha=0.8),
        mpatches.Patch(facecolor='#2B4865', label="Water", alpha=0.8),
    ]
    leg = ax.legend(handles=legend_items, loc='lower right', fontsize=7,
                    facecolor='#1a1a2e', edgecolor='white', labelcolor='white',
                    framealpha=0.9)

    # Grid
    for g in np.arange(0, 1.01, 0.125):
        ax.axhline(g, color='white', alpha=0.08, linewidth=0.5)
        ax.axvline(g, color='white', alpha=0.08, linewidth=0.5)

    # Scale bar
    ax.plot([0.82, 0.95], [0.97, 0.97], color='white', linewidth=2)
    ax.text(0.885, 0.96, '1 km', ha='center', va='bottom', fontsize=7, color='white')

    # Stats box
    stats = "Erangel Stats\n─────────────\nSize: 8×8 km\nLand: 51.47%\nPOIs: 21\nBuildings: ~460\nElevation: 0-200m"
    ax.text(0.01, 0.99, stats, transform=ax.transAxes, fontsize=7,
            verticalalignment='top', color='white', family='monospace',
            bbox=dict(boxstyle='round', facecolor='#1a1a2e', alpha=0.8, edgecolor='white'))

    ax.tick_params(colors='white', labelsize=6)
    for spine in ax.spines.values():
        spine.set_color('white')

    out_path = r'e:\EEE\Shot1\S1\files\erangel_expected_output.png'
    plt.savefig(out_path, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close()
    print(f"Saved: {out_path}")


if __name__ == "__main__":
    main()
