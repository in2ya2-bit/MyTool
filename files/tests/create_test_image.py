"""Create a simple test floor plan image for parser testing."""
import numpy as np

try:
    import cv2
except ImportError:
    print("cv2 not available, skipping image creation")
    exit(0)


def create_simple_office_image():
    """Simple office: 20m x 12m at 50 pixels/meter = 1000x600 image."""
    ppm = 50
    w_m, h_m = 20, 12
    w_px, h_px = int(w_m * ppm), int(h_m * ppm)
    padding = 50

    img = np.ones((h_px + padding * 2, w_px + padding * 2), dtype=np.uint8) * 255

    ox, oy = padding, padding
    thickness = 3

    cv2.rectangle(img, (ox, oy), (ox + w_px, oy + h_px), 0, thickness)

    mid_x = ox + w_px // 2
    cv2.line(img, (mid_x, oy), (mid_x, oy + h_px), 0, 2)

    mid_y = oy + h_px // 2
    cv2.line(img, (ox, mid_y), (mid_x, mid_y), 0, 2)
    cv2.line(img, (mid_x, mid_y), (ox + w_px, mid_y), 0, 2)

    path = "e:\\EEE\\Shot1\\S1\\files\\samples\\test_office_image.png"
    cv2.imwrite(path, img)
    print(f"Created: {path}  ({img.shape[1]}x{img.shape[0]})")
    return path


if __name__ == "__main__":
    create_simple_office_image()
