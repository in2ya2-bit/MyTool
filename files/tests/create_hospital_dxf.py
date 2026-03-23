"""Create a hospital-like DXF floor plan for parser testing.

Layout (35m x 20m):
  - Central corridor (35m x 3m)
  - North side: 5 patient rooms + 1 nurse station
  - South side: 4 patient rooms + 1 surgery + 1 utility
  - East end: stairwell area
  - Doors on every room, windows on exterior walls
"""
import ezdxf
import os


def create_hospital():
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add("WALL_EXT", color=1)
    doc.layers.add("WALL_INT", color=3)
    doc.layers.add("DOOR", color=5)
    doc.layers.add("WINDOW", color=4)

    W, D = 35, 20
    corr_y1 = 8.5
    corr_y2 = 11.5

    ext = [((0, 0), (W, 0)), ((W, 0), (W, D)), ((W, D), (0, D)), ((0, D), (0, 0))]
    for s, e in ext:
        msp.add_line(s, e, dxfattribs={"layer": "WALL_EXT"})

    msp.add_line((0, corr_y1), (W, corr_y1), dxfattribs={"layer": "WALL_INT"})
    msp.add_line((0, corr_y2), (W, corr_y2), dxfattribs={"layer": "WALL_INT"})

    south_divs = [7, 14, 21, 26, 30]
    for x in south_divs:
        msp.add_line((x, 0), (x, corr_y1), dxfattribs={"layer": "WALL_INT"})

    north_divs = [6, 12, 18, 24, 29]
    for x in north_divs:
        msp.add_line((x, corr_y2), (x, D), dxfattribs={"layer": "WALL_INT"})

    door_block = doc.blocks.new(name="DOOR_SINGLE")
    door_block.add_line((0, 0), (1.0, 0))

    south_door_xs = [3.5, 10.5, 17.5, 23.5, 28, 32.5]
    for x in south_door_xs:
        msp.add_blockref("DOOR_SINGLE", (x, corr_y1), dxfattribs={"layer": "DOOR"})

    north_door_xs = [3, 9, 15, 21, 26.5, 32]
    for x in north_door_xs:
        msp.add_blockref("DOOR_SINGLE", (x, corr_y2), dxfattribs={"layer": "DOOR"})

    win_block = doc.blocks.new(name="WINDOW_1500")
    win_block.add_line((0, 0), (1.5, 0))

    for x in [3, 10, 17, 24, 31]:
        msp.add_blockref("WINDOW_1500", (x, 0),
                         dxfattribs={"layer": "WINDOW", "xscale": 1.5})

    for x in [3, 9, 15, 21, 27, 32]:
        msp.add_blockref("WINDOW_1500", (x, D),
                         dxfattribs={"layer": "WINDOW", "xscale": 1.5})

    out_dir = os.path.join(os.path.dirname(__file__), "..", "samples")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "hospital_floor.dxf")
    doc.saveas(path)
    print(f"Created: {path}")
    return path


if __name__ == "__main__":
    create_hospital()
