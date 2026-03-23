"""Create a simple test DXF for parser testing."""
import ezdxf
import os

def create_simple_office():
    """Simple office: 20m x 12m exterior, 4 rooms, 2 doors, 3 windows."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add("WALL_EXT", color=1)
    doc.layers.add("WALL_INT", color=3)
    doc.layers.add("DOOR", color=5)
    doc.layers.add("WINDOW", color=4)
    doc.layers.add("COLUMN", color=6)

    ext_walls = [
        ((0, 0), (20, 0)),
        ((20, 0), (20, 12)),
        ((20, 12), (0, 12)),
        ((0, 12), (0, 0)),
    ]
    for s, e in ext_walls:
        msp.add_line(s, e, dxfattribs={"layer": "WALL_EXT"})

    int_walls = [
        ((10, 0), (10, 12)),
        ((0, 6), (10, 6)),
        ((10, 6), (20, 6)),
    ]
    for s, e in int_walls:
        msp.add_line(s, e, dxfattribs={"layer": "WALL_INT"})

    msp.add_circle((5, 5), 0.2, dxfattribs={"layer": "COLUMN"})
    msp.add_circle((15, 5), 0.2, dxfattribs={"layer": "COLUMN"})

    out_dir = os.path.join(os.path.dirname(__file__), "..", "samples")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "test_office.dxf")
    doc.saveas(path)
    print(f"Created: {path}")

    return path


def create_L_shape_building():
    """L-shape building with polyline exterior."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add("WALL_EXT", color=1)
    doc.layers.add("WALL_INT", color=3)

    pts = [(0, 0), (20, 0), (20, 8), (12, 8), (12, 15), (0, 15)]
    msp.add_lwpolyline(pts, close=True, dxfattribs={"layer": "WALL_EXT"})

    msp.add_line((0, 8), (12, 8), dxfattribs={"layer": "WALL_INT"})
    msp.add_line((8, 0), (8, 8), dxfattribs={"layer": "WALL_INT"})
    msp.add_line((6, 8), (6, 15), dxfattribs={"layer": "WALL_INT"})

    out_dir = os.path.join(os.path.dirname(__file__), "..", "samples")
    path = os.path.join(out_dir, "test_L_shape.dxf")
    doc.saveas(path)
    print(f"Created: {path}")

    return path


if __name__ == "__main__":
    create_simple_office()
    create_L_shape_building()
    print("All test DXFs created.")
