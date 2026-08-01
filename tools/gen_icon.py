"""appicon.ico 생성기.

디자인: 어두운 둥근 타일 위의 도넛형 진행률 링.
trayapp.cpp 의 TrayApp::makeIcon() 이 같은 도형을 QPainter 로 그린다.
둘 중 하나를 고치면 다른 쪽도 맞춰야 한다.

앱 아이콘은 '정체성'이므로 코랄 고정색을 쓰고,
트레이 아이콘은 '상태'이므로 사용량에 따라 색이 바뀐다(quotapanel 과 동일 팔레트).

사용법:  python tools/gen_icon.py
필요:    pillow
"""
import math
import os
from PIL import Image, ImageDraw

SS = 4          # 슈퍼샘플링 배율 (PIL 은 안티앨리어싱을 하지 않는다)
UNIT = 64.0     # 논리 좌표계

TILE = (0x26, 0x26, 0x2D, 255)
# 트랙은 반투명 흰색이 아니라 '타일 위에 흰색 22% 를 올린 결과'를 미리 계산한
# 불투명 색이다. PIL 의 ImageDraw 는 알파를 블렌딩하지 않고 덮어쓰기 때문에
# 반투명색을 그대로 쓰면 배경이 비쳐 흰 링으로 보인다.
TRACK = (0x55, 0x55, 0x5A, 255)
CORAL = (0xF4, 0x8A, 0x6A, 255)

TILE_RADIUS = 15.0
RING_INSET = 12.0    # 타일 가장자리에서 링 중심까지의 여백
RING_WIDTH = 9.5
APP_FILL = 0.72      # 앱 아이콘이 보여줄 고정 사용량


def _arc(d, box, start_deg, end_deg, width, color):
    """둥근 끝(Qt::RoundCap 상당)을 가진 호."""
    if end_deg <= start_deg:
        return
    d.arc(box, start_deg, end_deg, fill=color, width=int(round(width)))
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    r_out = (box[2] - box[0]) / 2.0
    r = width / 2.0
    for ang in (start_deg, end_deg):
        a = math.radians(ang)
        x, y = cx + r_out * math.cos(a), cy + r_out * math.sin(a)
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def render(px, fill=APP_FILL, color=CORAL):
    S = px * SS
    k = S / UNIT
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=TILE_RADIUS * k, fill=TILE)

    m = RING_INSET * k
    box = [m, m, S - m, S - m]
    w = RING_WIDTH * k
    _arc(d, box, 0, 360, w, TRACK)
    # 12시에서 시계방향. PIL 각도는 3시가 0도이고 시계방향이 양수다.
    _arc(d, box, -90, -90 + 360 * fill, w, color)

    return img.resize((px, px), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [render(s) for s in sizes]
    out = os.path.join(root, "appicon.ico")
    imgs[0].save(out, format="ICO", sizes=[(s, s) for s in sizes],
                 append_images=imgs[1:])
    print("wrote", out)


if __name__ == "__main__":
    main()
