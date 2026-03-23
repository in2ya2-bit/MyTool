"""
Erangel Reference Map — 3D Heightmap Visualization
Shows terrain elevation with POI markers and water.
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import numpy as np
from matplotlib.path import Path

MAP_SIZE_KM = 8

POIS = [
    {"name": "Military Base", "cx": 0.52, "cy": 0.85, "r": 500, "tier": "S", "elev": 5, "hint": "flat", "island": True},
    {"name": "Pochinki",      "cx": 0.42, "cy": 0.52, "r": 300, "tier": "S", "elev": 30, "hint": "flat"},
    {"name": "School",        "cx": 0.47, "cy": 0.38, "r": 100, "tier": "S", "elev": 35, "hint": "flat"},
    {"name": "Georgopol",     "cx": 0.18, "cy": 0.35, "r": 400, "tier": "A", "elev": 10, "hint": "coastal"},
    {"name": "Yasnaya P.",    "cx": 0.72, "cy": 0.28, "r": 350, "tier": "A", "elev": 50, "hint": "gentle_hill"},
    {"name": "Mylta Power",   "cx": 0.85, "cy": 0.65, "r": 150, "tier": "A", "elev": 5,  "hint": "coastal"},
    {"name": "Novorepnoye",   "cx": 0.88, "cy": 0.82, "r": 200, "tier": "A", "elev": 3,  "hint": "coastal"},
    {"name": "Rozhok",        "cx": 0.45, "cy": 0.35, "r": 200, "tier": "B", "elev": 40, "hint": "gentle_hill"},
    {"name": "Prison",        "cx": 0.65, "cy": 0.60, "r": 120, "tier": "B", "elev": 80, "hint": "hilltop"},
    {"name": "Stalber",       "cx": 0.72, "cy": 0.12, "r": 80,  "tier": "C", "elev": 180,"hint": "hilltop"},
    {"name": "Zharki",        "cx": 0.08, "cy": 0.08, "r": 150, "tier": "C", "elev": 5,  "hint": "coastal"},
    {"name": "Severny",       "cx": 0.30, "cy": 0.08, "r": 150, "tier": "C", "elev": 5,  "hint": "coastal"},
    {"name": "Quarry",        "cx": 0.18, "cy": 0.68, "r": 120, "tier": "C", "elev": -10,"hint": "valley"},
    {"name": "Primorsk",      "cx": 0.18, "cy": 0.78, "r": 200, "tier": "B", "elev": 5,  "hint": "coastal"},
    {"name": "Lipovka",       "cx": 0.80, "cy": 0.42, "r": 150, "tier": "C", "elev": 10, "hint": "coastal"},
    {"name": "Kameshki",      "cx": 0.92, "cy": 0.10, "r": 120, "tier": "C", "elev": 5,  "hint": "coastal"},
    {"name": "Gatka",         "cx": 0.25, "cy": 0.55, "r": 150, "tier": "C", "elev": 20, "hint": "flat"},
    {"name": "Hospital",      "cx": 0.13, "cy": 0.48, "r": 100, "tier": "B", "elev": 20, "hint": "flat"},
    {"name": "Mansion",       "cx": 0.72, "cy": 0.52, "r": 80,  "tier": "B", "elev": 50, "hint": "gentle_hill"},
    {"name": "Shooting R.",   "cx": 0.35, "cy": 0.15, "r": 100, "tier": "B", "elev": 60, "hint": "gentle_hill"},
    {"name": "Ferry Pier",    "cx": 0.35, "cy": 0.82, "r": 100, "tier": "B", "elev": 2,  "hint": "coastal"},
]

TIER_COLORS = {"S": "#FF3333", "A": "#FF9933", "B": "#3399FF", "C": "#88CC88"}


def make_island_mask(X, Y):
    """Create main island + military island masks."""
    t = np.linspace(0, 2*np.pi, 200)

    main_r = 0.34 + 0.06*np.sin(3*t) + 0.04*np.cos(5*t) + 0.03*np.sin(7*t)
    main_x = 0.44 + main_r * np.cos(t) * 1.15
    main_y = 0.38 + main_r * np.sin(t) * 1.05
    main_x = np.clip(main_x, 0.02, 0.98)
    main_y = np.clip(main_y, 0.02, 0.72)

    t2 = np.linspace(0, 2*np.pi, 100)
    mil_r = 0.12 + 0.02*np.sin(4*t2)
    mil_x = 0.52 + mil_r * np.cos(t2) * 1.3
    mil_y = 0.84 + mil_r * np.sin(t2) * 0.6

    pts = np.column_stack([X.ravel(), Y.ravel()])

    main_path = Path(np.column_stack([main_x, main_y]))
    mil_path = Path(np.column_stack([mil_x, mil_y]))

    mask = main_path.contains_points(pts) | mil_path.contains_points(pts)
    return mask.reshape(X.shape), main_path, mil_path


def generate_heightmap(X, Y, mask):
    """Generate terrain heightmap with POI elevation hints."""
    res = X.shape[0]
    Z = np.full_like(X, -15.0)

    base = np.zeros_like(X)
    np.random.seed(42)
    for _ in range(8):
        freq_x = np.random.uniform(2, 6)
        freq_y = np.random.uniform(2, 6)
        phase_x = np.random.uniform(0, 2*np.pi)
        phase_y = np.random.uniform(0, 2*np.pi)
        amp = np.random.uniform(10, 40)
        base += amp * np.sin(freq_x * X * 2*np.pi + phase_x) * np.cos(freq_y * Y * 2*np.pi + phase_y)

    base = (base - base.min()) / (base.max() - base.min()) * 120 + 5

    for poi in POIS:
        cx, cy, elev = poi["cx"], poi["cy"], poi["elev"]
        r_km = poi["r"] / 1000.0
        r_norm = r_km / MAP_SIZE_KM * 2.5
        hint = poi["hint"]

        dist = np.sqrt((X - cx)**2 + (Y - cy)**2)
        influence = np.exp(-(dist / r_norm)**2)

        if hint == "hilltop":
            base += influence * max(elev - 30, 40)
        elif hint == "valley":
            base -= influence * 50
        elif hint == "coastal":
            base = base * (1 - influence * 0.5) + influence * elev * 0.5
        elif hint == "flat":
            flat_mask = influence > 0.3
            base[flat_mask] = base[flat_mask] * 0.3 + elev * 0.7
        elif hint == "gentle_hill":
            base += influence * max(elev * 0.3, 10)

    base = np.clip(base, 0, 200)

    Z[mask] = base[mask]

    from scipy.ndimage import gaussian_filter
    Z_smooth = gaussian_filter(Z, sigma=2)
    Z[mask] = Z_smooth[mask]

    coast_dist = np.ones_like(X) * 999
    for i in range(res):
        for j in range(res):
            if mask[i, j]:
                for di in range(-5, 6):
                    for dj in range(-5, 6):
                        ni, nj = i+di, j+dj
                        if 0 <= ni < res and 0 <= nj < res and not mask[ni, nj]:
                            d = np.sqrt(di**2 + dj**2)
                            coast_dist[i, j] = min(coast_dist[i, j], d)

    coast_blend = np.clip(coast_dist / 4.0, 0, 1)
    Z[mask] = Z[mask] * coast_blend[mask]

    return Z


def main():
    res = 300
    x = np.linspace(0, 1, res)
    y = np.linspace(0, 1, res)
    X, Y = np.meshgrid(x, y)

    print("Generating island mask...")
    mask, main_path, mil_path = make_island_mask(X, Y)

    print("Generating heightmap with POI elevation hints...")
    Z = generate_heightmap(X, Y, mask)

    fig = plt.figure(figsize=(22, 10), facecolor='#0d1117')

    # ═══════════════════════════════════════════════════════
    # LEFT: 3D terrain view
    # ═══════════════════════════════════════════════════════
    ax3d = fig.add_subplot(121, projection='3d', facecolor='#0d1117')

    terrain_cmap = mcolors.LinearSegmentedColormap.from_list('terrain_custom', [
        (0.0,  '#1B3A4B'),   # deep water
        (0.07, '#2B5A7B'),   # shallow water
        (0.08, '#C2B280'),   # beach
        (0.12, '#5A7D4A'),   # low grass
        (0.30, '#4A6D3A'),   # grass
        (0.50, '#6B8E5A'),   # mid grass
        (0.70, '#8BAA6B'),   # light grass
        (0.85, '#AA9977'),   # rock
        (1.0,  '#CCBBAA'),   # peak
    ])

    Z_display = Z.copy()
    Z_display[~mask] = -15

    Z_norm = (Z_display - Z_display.min()) / (Z_display.max() - Z_display.min())

    surf = ax3d.plot_surface(X, Y, Z_display, facecolors=terrain_cmap(Z_norm),
                              rstride=2, cstride=2, antialiased=True, shade=True,
                              lightsource=matplotlib.colors.LightSource(azdeg=315, altdeg=35))

    water_Z = np.full_like(X, -12.0)
    water_color = np.full((*X.shape, 4), [0.1, 0.25, 0.45, 0.7])
    ax3d.plot_surface(X, Y, water_Z, facecolors=water_color,
                      rstride=4, cstride=4, antialiased=False)

    for poi in POIS:
        cx, cy, elev = poi["cx"], poi["cy"], max(poi["elev"], 5)
        color = TIER_COLORS[poi["tier"]]

        zi = int(cy * (res-1))
        xi = int(cx * (res-1))
        zi = np.clip(zi, 0, res-1)
        xi = np.clip(xi, 0, res-1)
        actual_z = Z[zi, xi] if mask[zi, xi] else 5

        ax3d.scatter([cx], [cy], [actual_z + 8], color=color, s=40,
                     edgecolors='white', linewidth=0.8, zorder=10, depthshade=False)

        if poi["tier"] in ("S", "A"):
            ax3d.text(cx, cy, actual_z + 15, poi["name"],
                     fontsize=6, color='white', fontweight='bold',
                     ha='center', va='bottom', zorder=11)

    ax3d.set_xlim(0, 1)
    ax3d.set_ylim(0, 1)
    ax3d.set_zlim(-20, 200)
    ax3d.view_init(elev=55, azim=-60)
    ax3d.set_xlabel('X (8km)', color='gray', fontsize=8)
    ax3d.set_ylabel('Y (8km)', color='gray', fontsize=8)
    ax3d.set_zlabel('Elevation (m)', color='gray', fontsize=8)
    ax3d.set_title('3D Terrain with Elevation', color='white', fontsize=13,
                   fontweight='bold', pad=10)
    ax3d.tick_params(colors='gray', labelsize=6)
    ax3d.xaxis.pane.fill = False
    ax3d.yaxis.pane.fill = False
    ax3d.zaxis.pane.fill = False
    ax3d.xaxis.pane.set_edgecolor('gray')
    ax3d.yaxis.pane.set_edgecolor('gray')
    ax3d.zaxis.pane.set_edgecolor('gray')

    # ═══════════════════════════════════════════════════════
    # RIGHT: Top-down heightmap view
    # ═══════════════════════════════════════════════════════
    ax2d = fig.add_subplot(122, facecolor='#1B3A4B')
    ax2d.set_aspect('equal')

    Z_top = Z.copy()
    Z_top[~mask] = np.nan

    im = ax2d.imshow(Z_top, extent=[0, 1, 1, 0], cmap=terrain_cmap,
                     vmin=-15, vmax=200, interpolation='bilinear')

    water_display = np.full_like(Z, np.nan)
    water_display[~mask] = 1
    ax2d.imshow(water_display, extent=[0, 1, 1, 0],
                cmap=mcolors.LinearSegmentedColormap.from_list('w', ['#1B3A4B', '#2B5A7B']),
                alpha=0.9, interpolation='bilinear')

    for poi in POIS:
        cx, cy = poi["cx"], poi["cy"]
        color = TIER_COLORS[poi["tier"]]
        elev = poi["elev"]

        ax2d.plot(cx, cy, 'o', color=color, markersize=6,
                  markeredgecolor='white', markeredgewidth=0.8)

        label = f"{poi['name']}\n{elev}m"
        fontsize = 7 if poi["tier"] in ("S", "A") else 5.5
        ax2d.annotate(label, (cx, cy), textcoords="offset points",
                     xytext=(8, -5), fontsize=fontsize, color='white',
                     fontweight='bold' if poi["tier"] in ("S",) else 'normal',
                     bbox=dict(boxstyle='round,pad=0.15', facecolor=color, alpha=0.5, edgecolor='none'))

    cbar = plt.colorbar(im, ax=ax2d, fraction=0.046, pad=0.04, shrink=0.8)
    cbar.set_label('Elevation (m)', color='white', fontsize=9)
    cbar.ax.tick_params(colors='white', labelsize=7)

    elev_notes = (
        "Elevation Hints Applied:\n"
        "─────────────────────\n"
        "Stalber (hilltop): 180m peak\n"
        "Prison (hilltop): 80m ridge\n"
        "Shooting R. (gentle_hill): 60m\n"
        "Yasnaya P. (gentle_hill): 50m\n"
        "Rozhok (gentle_hill): 40m\n"
        "Pochinki (flat): 30m plateau\n"
        "Quarry (valley): -10m basin\n"
        "Coastal POIs: 3~10m sea level\n"
        "Military Base (flat island): 5m"
    )
    ax2d.text(0.02, 0.98, elev_notes, transform=ax2d.transAxes,
             fontsize=6.5, color='white', family='monospace',
             verticalalignment='top',
             bbox=dict(boxstyle='round', facecolor='#0d1117', alpha=0.85, edgecolor='gray'))

    ax2d.set_title('Top-Down Heightmap + POI Elevations', color='white',
                   fontsize=13, fontweight='bold', pad=10)
    ax2d.set_xlim(0, 1)
    ax2d.set_ylim(1, 0)
    ax2d.tick_params(colors='gray', labelsize=6)
    for spine in ax2d.spines.values():
        spine.set_color('gray')

    fig.suptitle('Erangel — Expected Terrain Generation Result (0~200m elevation range)',
                 color='#FFD700', fontsize=15, fontweight='bold', y=0.98)

    out = r'e:\EEE\Shot1\S1\files\erangel_3d_heightmap.png'
    plt.savefig(out, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close()
    print(f"Saved: {out}")


if __name__ == "__main__":
    main()
