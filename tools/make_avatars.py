"""Draws Sanctum's profile icons: assets/avatars.png (an atlas) and assets/avatars.txt (category and name per cell).

Every icon is original artwork drawn here from shapes (no game art). Each one is drawn in white and see-through tones:
light (white, part see-through) and shade / deep (black, part see-through). Sanctum lays the icon over the background
colour the player picks, so the same icon looks right on every colour and its shading takes that colour's tone.

    python tools/make_avatars.py            # writes assets/avatars.png, assets/avatars.txt
    python tools/make_avatars.py --sheet x.png   # also a contact sheet on sample colours (for checking by eye)
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

CELL = 192          # one icon in the atlas
SS = 4              # drawn 4x larger, then scaled down: smooth edges
N = CELL * SS
COLS = 8

W = (255, 255, 255, 255)   # main
L = (255, 255, 255, 150)   # light
D = (0, 0, 0, 95)          # shade
K = (0, 0, 0, 175)         # deep (eyes, holes)
CLEAR = (0, 0, 0, 0)


class Pen:
    """Unit-square drawing (0..1) on one RGBA layer. Later shapes replace what is under them (no blending), so a
    shade drawn over white becomes a darker tone of the background, not grey."""

    def __init__(self):
        self.im = Image.new("RGBA", (N, N), CLEAR)
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def p(x, y):
        return (x * N, y * N)

    def ell(self, cx, cy, rx, ry, c):
        self.d.ellipse([(cx - rx) * N, (cy - ry) * N, (cx + rx) * N, (cy + ry) * N], fill=c)

    def circ(self, cx, cy, r, c):
        self.ell(cx, cy, r, r, c)

    def ring(self, cx, cy, r, w, c):
        self.d.ellipse([(cx - r) * N, (cy - r) * N, (cx + r) * N, (cy + r) * N], outline=c, width=max(1, int(w * N)))

    def poly(self, pts, c):
        self.d.polygon([self.p(x, y) for x, y in pts], fill=c)

    def rr(self, x0, y0, x1, y1, r, c):
        self.d.rounded_rectangle([x0 * N, y0 * N, x1 * N, y1 * N], radius=r * N, fill=c)

    def line(self, pts, w, c, caps=True):
        q = [self.p(x, y) for x, y in pts]
        self.d.line(q, fill=c, width=max(1, int(w * N)), joint="curve")
        if caps:
            for x, y in pts:
                self.circ(x, y, w / 2, c)

    def arc(self, cx, cy, r, a0, a1, w, c, steps=48):
        pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
                cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]
        self.line(pts, w, c)

    def pie(self, cx, cy, r, a0, a1, c):
        self.d.pieslice([(cx - r) * N, (cy - r) * N, (cx + r) * N, (cy + r) * N], a0, a1, fill=c)

    def star(self, cx, cy, R, r, n, c, rot=-90):
        pts = []
        for i in range(n * 2):
            a = math.radians(rot + i * 180 / n)
            rad = R if i % 2 == 0 else r
            pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
        self.poly(pts, c)

    def soft_star(self, cx, cy, R, r, n, c, rot=-90):
        """a star with rounded points"""
        self.star(cx, cy, R, r, n, c, rot)
        for i in range(n):
            a = math.radians(rot + i * 360 / n)
            self.circ(cx + (R - 0.012) * math.cos(a), cy + (R - 0.012) * math.sin(a), 0.016, c)


def eyes(g, y, dx, r, c=K, cx=0.5, shine=True):
    for s in (-1, 1):
        g.circ(cx + s * dx, y, r, c)
        if shine and r > 0.02:
            g.circ(cx + s * dx + r * 0.35, y - r * 0.35, r * 0.32, W)


# ---------------------------------------------------------------- SteelTemple
def sword(g):
    g.poly([(0.5, 0.14), (0.545, 0.22), (0.545, 0.6), (0.455, 0.6), (0.455, 0.22)], W)
    g.poly([(0.5, 0.17), (0.5, 0.58), (0.455, 0.58), (0.455, 0.22)], L)
    g.rr(0.32, 0.6, 0.68, 0.655, 0.025, W)
    g.circ(0.32, 0.627, 0.035, W); g.circ(0.68, 0.627, 0.035, W)
    g.rr(0.468, 0.655, 0.532, 0.8, 0.015, L)
    g.circ(0.5, 0.83, 0.045, W)
    g.circ(0.5, 0.627, 0.022, D)


def shield(g):
    pts = [(0.5, 0.15), (0.78, 0.25), (0.76, 0.52)]
    pts += [(0.5 + 0.26 * math.cos(math.radians(a)) * (1 - (a - 0) / 260), 0.52 + 0.33 * math.sin(math.radians(a)))
            for a in range(0, 91, 10)]
    left = [(1 - x, y) for x, y in reversed(pts)]
    g.poly(pts + left, W)
    g.poly([(0.5, 0.15), (0.5, 0.85), (0.3, 0.7), (0.24, 0.52), (0.22, 0.25)], L)
    g.star(0.5, 0.48, 0.13, 0.055, 4, D, rot=-90)


def crown(g):
    g.poly([(0.2, 0.36), (0.35, 0.52), (0.5, 0.28), (0.65, 0.52), (0.8, 0.36), (0.75, 0.7), (0.25, 0.7)], W)
    for x, y in ((0.2, 0.34), (0.5, 0.25), (0.8, 0.34)):
        g.circ(x, y, 0.04, W)
    g.rr(0.25, 0.7, 0.75, 0.78, 0.02, L)
    for x in (0.36, 0.5, 0.64):
        g.circ(x, 0.6, 0.032, D)


def gem(g):
    g.poly([(0.3, 0.3), (0.7, 0.3), (0.84, 0.45), (0.5, 0.84), (0.16, 0.45)], W)
    g.poly([(0.16, 0.45), (0.84, 0.45), (0.5, 0.84)], L)
    g.poly([(0.38, 0.45), (0.62, 0.45), (0.5, 0.84)], W)
    g.poly([(0.3, 0.3), (0.38, 0.45), (0.16, 0.45)], L)
    g.poly([(0.7, 0.3), (0.84, 0.45), (0.62, 0.45)], D)
    g.poly([(0.5, 0.3), (0.62, 0.45), (0.38, 0.45)], L)


def key(g):
    g.circ(0.36, 0.38, 0.17, W)
    g.circ(0.36, 0.38, 0.075, K)
    g.line([(0.46, 0.5), (0.76, 0.8)], 0.075, W)
    g.line([(0.62, 0.66), (0.7, 0.58)], 0.06, W)
    g.line([(0.7, 0.74), (0.78, 0.66)], 0.06, W)


def potion(g):
    g.rr(0.44, 0.16, 0.56, 0.32, 0.02, L)
    g.rr(0.41, 0.14, 0.59, 0.19, 0.02, W)
    g.circ(0.5, 0.6, 0.27, W)
    g.rr(0.42, 0.28, 0.58, 0.4, 0.02, W)
    g.pie(0.5, 0.6, 0.235, 0, 180, L)
    g.rr(0.265, 0.585, 0.735, 0.62, 0.0, L)
    for x, y, r in ((0.43, 0.7, 0.03), (0.56, 0.74, 0.02), (0.52, 0.66, 0.015)):
        g.circ(x, y, r, W)
    g.ell(0.4, 0.48, 0.035, 0.06, L)


def temple(g):
    g.poly([(0.5, 0.16), (0.84, 0.36), (0.16, 0.36)], W)
    g.poly([(0.5, 0.24), (0.68, 0.34), (0.32, 0.34)], D)
    g.rr(0.18, 0.37, 0.82, 0.42, 0.01, L)
    for x in (0.25, 0.4, 0.6, 0.75):
        g.rr(x - 0.04, 0.44, x + 0.04, 0.74, 0.01, W)
    g.rr(0.14, 0.76, 0.86, 0.84, 0.015, W)


def compass(g):
    g.circ(0.5, 0.5, 0.34, W)
    g.circ(0.5, 0.5, 0.27, D)
    g.poly([(0.5, 0.25), (0.565, 0.5), (0.435, 0.5)], W)
    g.poly([(0.5, 0.75), (0.565, 0.5), (0.435, 0.5)], L)
    g.circ(0.5, 0.5, 0.035, W)
    for a in range(0, 360, 90):
        x, y = 0.5 + 0.305 * math.cos(math.radians(a)), 0.5 + 0.305 * math.sin(math.radians(a))
        g.circ(x, y, 0.016, L)


# ---------------------------------------------------------------- creatures
def cat(g):
    g.poly([(0.22, 0.2), (0.42, 0.34), (0.26, 0.48)], W)
    g.poly([(0.78, 0.2), (0.58, 0.34), (0.74, 0.48)], W)
    g.poly([(0.26, 0.26), (0.37, 0.34), (0.28, 0.42)], L)
    g.poly([(0.74, 0.26), (0.63, 0.34), (0.72, 0.42)], L)
    g.ell(0.5, 0.55, 0.3, 0.26, W)
    eyes(g, 0.52, 0.12, 0.04)
    g.poly([(0.47, 0.6), (0.53, 0.6), (0.5, 0.635)], D)
    g.arc(0.475, 0.64, 0.025, 0, 140, 0.012, D)
    g.arc(0.525, 0.64, 0.025, 40, 180, 0.012, D)
    for s in (-1, 1):
        g.line([(0.5 + s * 0.17, 0.62), (0.5 + s * 0.31, 0.6)], 0.012, L)
        g.line([(0.5 + s * 0.17, 0.65), (0.5 + s * 0.3, 0.67)], 0.012, L)


def fox(g):
    g.poly([(0.2, 0.18), (0.4, 0.34), (0.24, 0.5)], W)
    g.poly([(0.8, 0.18), (0.6, 0.34), (0.76, 0.5)], W)
    g.poly([(0.24, 0.26), (0.35, 0.35), (0.26, 0.44)], D)
    g.poly([(0.76, 0.26), (0.65, 0.35), (0.74, 0.44)], D)
    g.poly([(0.2, 0.42), (0.36, 0.32), (0.64, 0.32), (0.8, 0.42), (0.5, 0.84)], W)
    g.poly([(0.2, 0.42), (0.38, 0.56), (0.5, 0.84)], L)
    g.poly([(0.8, 0.42), (0.62, 0.56), (0.5, 0.84)], L)
    eyes(g, 0.5, 0.11, 0.032)
    g.circ(0.5, 0.79, 0.035, K)


def owl(g):
    g.ell(0.5, 0.56, 0.3, 0.32, W)
    g.poly([(0.22, 0.24), (0.36, 0.3), (0.26, 0.4)], W)
    g.poly([(0.78, 0.24), (0.64, 0.3), (0.74, 0.4)], W)
    for s in (-1, 1):
        g.circ(0.5 + s * 0.12, 0.46, 0.1, L)
        g.circ(0.5 + s * 0.12, 0.46, 0.062, K)
        g.circ(0.5 + s * 0.12 + 0.02, 0.44, 0.02, W)
    g.poly([(0.46, 0.56), (0.54, 0.56), (0.5, 0.64)], D)
    for x in (0.4, 0.5, 0.6):
        g.arc(x, 0.72, 0.035, 20, 160, 0.014, D)
    g.ell(0.5, 0.88, 0.12, 0.02, L)


def bear(g):
    for s in (-1, 1):
        g.circ(0.5 + s * 0.22, 0.28, 0.1, W)
        g.circ(0.5 + s * 0.22, 0.28, 0.05, D)
    g.circ(0.5, 0.55, 0.3, W)
    eyes(g, 0.49, 0.11, 0.032)
    g.ell(0.5, 0.65, 0.12, 0.09, L)
    g.ell(0.5, 0.61, 0.045, 0.03, K)
    g.line([(0.5, 0.63), (0.5, 0.68)], 0.014, D, caps=False)


def bunny(g):
    for s in (-1, 1):
        g.ell(0.5 + s * 0.11, 0.27, 0.07, 0.17, W)
        g.ell(0.5 + s * 0.11, 0.28, 0.032, 0.12, L)
    g.ell(0.5, 0.62, 0.27, 0.23, W)
    eyes(g, 0.59, 0.11, 0.03)
    g.ell(0.5, 0.67, 0.03, 0.02, D)
    for s in (-1, 1):
        g.circ(0.5 + s * 0.17, 0.68, 0.035, L)


def frog(g):
    for s in (-1, 1):
        g.circ(0.5 + s * 0.17, 0.34, 0.11, W)
        g.circ(0.5 + s * 0.17, 0.34, 0.055, K)
        g.circ(0.5 + s * 0.17 + 0.02, 0.32, 0.018, W)
    g.ell(0.5, 0.6, 0.32, 0.22, W)
    g.arc(0.5, 0.56, 0.17, 25, 155, 0.022, D)
    for s in (-1, 1):
        g.circ(0.5 + s * 0.22, 0.64, 0.035, L)
    g.circ(0.47, 0.5, 0.01, D); g.circ(0.53, 0.5, 0.01, D)


def penguin(g):
    g.ell(0.5, 0.52, 0.27, 0.34, W)
    g.ell(0.5, 0.58, 0.18, 0.26, L)
    eyes(g, 0.4, 0.075, 0.03)
    g.poly([(0.45, 0.47), (0.55, 0.47), (0.5, 0.53)], D)
    g.ell(0.41, 0.87, 0.07, 0.03, W)
    g.ell(0.59, 0.87, 0.07, 0.03, W)
    g.ell(0.25, 0.58, 0.04, 0.15, W)
    g.ell(0.75, 0.58, 0.04, 0.15, W)


def panda(g):
    for s in (-1, 1):
        g.circ(0.5 + s * 0.23, 0.28, 0.09, D)
    g.circ(0.5, 0.55, 0.31, W)
    for s in (-1, 1):
        g.ell(0.5 + s * 0.12, 0.5, 0.07, 0.085, D)
        g.circ(0.5 + s * 0.12 + s * 0.01, 0.49, 0.028, K)
        g.circ(0.5 + s * 0.12 + s * 0.01 + 0.01, 0.48, 0.01, W)
    g.ell(0.5, 0.63, 0.04, 0.028, K)
    g.arc(0.48, 0.67, 0.02, 0, 160, 0.012, D)
    g.arc(0.52, 0.67, 0.02, 20, 180, 0.012, D)


def bird(g):
    g.circ(0.47, 0.52, 0.27, W)
    g.poly([(0.72, 0.46), (0.86, 0.52), (0.72, 0.58)], L)
    g.ell(0.38, 0.6, 0.15, 0.09, L)
    g.circ(0.55, 0.44, 0.035, K)
    g.circ(0.56, 0.43, 0.012, W)
    g.poly([(0.36, 0.26), (0.42, 0.14), (0.46, 0.26)], W)
    g.poly([(0.44, 0.26), (0.52, 0.16), (0.53, 0.27)], W)
    g.line([(0.42, 0.78), (0.42, 0.86)], 0.018, L)
    g.line([(0.52, 0.78), (0.52, 0.86)], 0.018, L)


def fish(g):
    g.ell(0.46, 0.5, 0.27, 0.19, W)
    g.poly([(0.68, 0.5), (0.86, 0.33), (0.86, 0.67)], W)
    g.poly([(0.36, 0.33), (0.5, 0.2), (0.56, 0.33)], L)
    g.arc(0.52, 0.5, 0.13, -60, 60, 0.02, D)
    g.circ(0.33, 0.46, 0.035, K)
    g.circ(0.34, 0.45, 0.012, W)
    for x, y, r in ((0.16, 0.32, 0.03), (0.2, 0.22, 0.02), (0.13, 0.15, 0.015)):
        g.ring(x, y, r, 0.01, L)


def octopus(g):
    g.ell(0.5, 0.38, 0.24, 0.23, W)
    for i in range(5):
        x = 0.3 + i * 0.1
        g.line([(x, 0.52), (x + (0.03 if i % 2 else -0.03), 0.68), (x, 0.82)], 0.06, W)
    eyes(g, 0.4, 0.09, 0.035)
    g.arc(0.5, 0.45, 0.05, 20, 160, 0.015, D)
    for x, y in ((0.39, 0.25), (0.6, 0.28)):
        g.circ(x, y, 0.025, L)


def ghost(g):
    g.pie(0.5, 0.45, 0.27, 180, 360, W)
    g.rr(0.23, 0.44, 0.77, 0.72, 0.0, W)
    for i in range(4):
        g.circ(0.2975 + i * 0.135, 0.72, 0.0675, W)
    eyes(g, 0.46, 0.09, 0.04)
    g.ell(0.5, 0.58, 0.035, 0.045, D)
    g.circ(0.33, 0.55, 0.03, L); g.circ(0.67, 0.55, 0.03, L)


# ---------------------------------------------------------------- nature
def leaf(g):
    pts = [(0.5 + 0.3 * math.sin(math.radians(a)) * math.sin(math.radians(a / 2)) * 1.0,
            0.5 - 0.33 * math.cos(math.radians(a))) for a in range(0, 361, 6)]
    pts = []
    for t in range(0, 181, 6):
        a = math.radians(t)
        pts.append((0.5 + 0.24 * math.sin(a), 0.16 + 0.62 * (1 - math.cos(a)) / 2))
    for t in range(180, -1, -6):
        a = math.radians(t)
        pts.append((0.5 - 0.24 * math.sin(a), 0.16 + 0.62 * (1 - math.cos(a)) / 2))
    g.poly(pts, W)
    g.poly([p for p in pts if p[0] <= 0.5], L)
    g.line([(0.5, 0.22), (0.5, 0.86)], 0.022, D)
    for y in (0.36, 0.5, 0.64):
        g.line([(0.5, y + 0.06), (0.66, y)], 0.016, D)
        g.line([(0.5, y + 0.06), (0.34, y)], 0.016, D)


def flower(g):
    for i in range(6):
        a = math.radians(i * 60 - 90)
        g.circ(0.5 + 0.17 * math.cos(a), 0.5 + 0.17 * math.sin(a), 0.14, W if i % 2 == 0 else L)
    g.circ(0.5, 0.5, 0.11, W)
    g.circ(0.5, 0.5, 0.08, D)
    for i in range(5):
        a = math.radians(i * 72)
        g.circ(0.5 + 0.035 * math.cos(a), 0.5 + 0.035 * math.sin(a), 0.013, L)


def tree(g):
    g.rr(0.46, 0.6, 0.54, 0.86, 0.02, L)
    g.circ(0.5, 0.36, 0.2, W)
    g.circ(0.34, 0.5, 0.15, W)
    g.circ(0.66, 0.5, 0.15, W)
    g.circ(0.5, 0.55, 0.15, W)
    for x, y in ((0.42, 0.33), (0.6, 0.45), (0.38, 0.55)):
        g.circ(x, y, 0.035, L)


def mountain(g):
    g.poly([(0.12, 0.8), (0.4, 0.3), (0.58, 0.6), (0.68, 0.46), (0.9, 0.8)], W)
    g.poly([(0.4, 0.3), (0.48, 0.44), (0.43, 0.42), (0.39, 0.47), (0.34, 0.41)], L)
    g.poly([(0.4, 0.3), (0.58, 0.6), (0.5, 0.8), (0.4, 0.8)], L)
    g.circ(0.74, 0.26, 0.07, W)


def wave(g):
    def band(y, amp, phase, c, bottom):
        pts = [(0.12 + 0.76 * t / 60, y + amp * math.sin(t / 60 * math.pi * 2.5 + phase)) for t in range(61)]
        g.poly(pts + [(0.88, bottom), (0.12, bottom)], c)
    g.d.ellipse([0.12 * N, 0.12 * N, 0.88 * N, 0.88 * N], fill=CLEAR)
    band(0.34, 0.05, 0.0, W, 0.5)
    band(0.5, 0.05, 1.2, L, 0.66)
    band(0.66, 0.05, 2.4, W, 0.84)
    # the waves sit in a circle so the bands end in a round shape
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).ellipse([0.14 * N, 0.14 * N, 0.86 * N, 0.86 * N], fill=255)
    a = g.im.getchannel("A")
    g.im.putalpha(Image.fromarray(__import__("numpy").minimum(__import__("numpy").array(a), __import__("numpy").array(mask))))
    g.circ(0.66, 0.24, 0.035, L)


def sun(g):
    for i in range(12):
        a = math.radians(i * 30)
        g.line([(0.5 + 0.27 * math.cos(a), 0.5 + 0.27 * math.sin(a)),
                (0.5 + 0.37 * math.cos(a), 0.5 + 0.37 * math.sin(a))], 0.045, W if i % 2 == 0 else L)
    g.circ(0.5, 0.5, 0.21, W)
    g.pie(0.5, 0.5, 0.21, 20, 160, L)


def moon(g):
    g.circ(0.47, 0.5, 0.31, W)
    g.circ(0.62, 0.4, 0.25, CLEAR)
    g.circ(0.3, 0.6, 0.04, L)
    g.circ(0.42, 0.72, 0.03, L)
    g.circ(0.27, 0.42, 0.025, L)
    g.soft_star(0.66, 0.5, 0.06, 0.025, 4, W)
    g.soft_star(0.78, 0.26, 0.04, 0.017, 4, L)


def snowflake(g):
    for i in range(6):
        a = math.radians(i * 60 - 90)
        x1, y1 = 0.5 + 0.35 * math.cos(a), 0.5 + 0.35 * math.sin(a)
        g.line([(0.5, 0.5), (x1, y1)], 0.04, W)
        for f in (0.55, 0.78):
            bx, by = 0.5 + 0.35 * f * math.cos(a), 0.5 + 0.35 * f * math.sin(a)
            for s in (-1, 1):
                b = a + s * math.radians(45)
                g.line([(bx, by), (bx + 0.09 * math.cos(b), by + 0.09 * math.sin(b))], 0.03, W)
    g.circ(0.5, 0.5, 0.07, W)
    g.star(0.5, 0.5, 0.05, 0.025, 6, D)


# ---------------------------------------------------------------- space
def star(g):
    g.soft_star(0.5, 0.52, 0.36, 0.16, 5, W)
    g.star(0.5, 0.52, 0.19, 0.085, 5, L)
    for x, y in ((0.18, 0.22), (0.82, 0.24), (0.8, 0.82)):
        g.soft_star(x, y, 0.04, 0.016, 4, L)


def planet(g):
    g.circ(0.5, 0.5, 0.22, W)
    g.pie(0.5, 0.5, 0.22, 200, 340, L)
    g.d.ellipse([0.1 * N, 0.42 * N, 0.9 * N, 0.58 * N], outline=W, width=int(0.035 * N))
    g.pie(0.5, 0.5, 0.2, 180, 360, W)
    g.pie(0.5, 0.5, 0.2, 200, 340, L)
    g.ell(0.44, 0.4, 0.05, 0.03, D)
    g.circ(0.6, 0.34, 0.02, D)
    g.soft_star(0.18, 0.22, 0.04, 0.016, 4, L)


def rocket(g):
    g.ell(0.5, 0.46, 0.13, 0.3, W)
    g.poly([(0.37, 0.56), (0.25, 0.72), (0.38, 0.7)], L)
    g.poly([(0.63, 0.56), (0.75, 0.72), (0.62, 0.7)], L)
    g.circ(0.5, 0.42, 0.06, K)
    g.circ(0.5, 0.42, 0.035, L)
    g.rr(0.44, 0.72, 0.56, 0.77, 0.01, D)
    g.poly([(0.45, 0.78), (0.55, 0.78), (0.5, 0.9)], L)
    g.poly([(0.47, 0.78), (0.53, 0.78), (0.5, 0.85)], W)


def comet(g):
    hx, hy = 0.62, 0.38
    px, py = 0.7071, 0.7071   # across the tail
    g.poly([(hx + px * 0.16, hy + py * 0.16), (hx - px * 0.16, hy - py * 0.16), (0.14, 0.86)], L)
    g.poly([(hx + px * 0.1, hy + py * 0.1), (hx - px * 0.1, hy - py * 0.1), (0.26, 0.74)], W)
    g.circ(hx, hy, 0.16, W)
    g.circ(0.58, 0.34, 0.04, L)
    g.circ(0.68, 0.44, 0.03, D)
    g.soft_star(0.84, 0.18, 0.04, 0.016, 4, L)


def ufo(g):
    g.pie(0.5, 0.48, 0.14, 180, 360, L)
    g.ell(0.5, 0.52, 0.34, 0.1, W)
    g.ell(0.5, 0.49, 0.2, 0.04, D)
    for x in (0.3, 0.42, 0.58, 0.7):
        g.circ(x, 0.56, 0.02, D)
    g.poly([(0.42, 0.62), (0.58, 0.62), (0.7, 0.88), (0.3, 0.88)], (255, 255, 255, 70))


def alien(g):
    g.line([(0.4, 0.3), (0.32, 0.16)], 0.025, W)
    g.line([(0.6, 0.3), (0.68, 0.16)], 0.025, W)
    g.circ(0.32, 0.16, 0.04, L); g.circ(0.68, 0.16, 0.04, L)
    g.ell(0.5, 0.52, 0.27, 0.3, W)
    for s in (-1, 1):
        g.ell(0.5 + s * 0.12, 0.5, 0.07, 0.095, K)
        g.circ(0.5 + s * 0.12 + 0.025, 0.46, 0.022, W)
    g.arc(0.5, 0.66, 0.05, 30, 150, 0.015, D)


# ---------------------------------------------------------------- play
def gamepad(g):
    g.rr(0.14, 0.36, 0.86, 0.66, 0.12, W)
    g.circ(0.26, 0.62, 0.13, W); g.circ(0.74, 0.62, 0.13, W)
    g.rr(0.24, 0.47, 0.36, 0.51, 0.008, D)
    g.rr(0.28, 0.43, 0.32, 0.55, 0.008, D)
    for x, y in ((0.7, 0.44), (0.76, 0.49), (0.64, 0.49), (0.7, 0.54)):
        g.circ(x, y, 0.025, D)
    g.rr(0.45, 0.45, 0.55, 0.48, 0.01, L)


def trophy(g):
    g.pie(0.5, 0.3, 0.22, 0, 180, W)
    g.rr(0.28, 0.2, 0.72, 0.31, 0.0, W)
    g.arc(0.27, 0.36, 0.08, 90, 270, 0.035, W)
    g.arc(0.73, 0.36, 0.08, -90, 90, 0.035, W)
    g.rr(0.46, 0.5, 0.54, 0.66, 0.0, L)
    g.rr(0.33, 0.66, 0.67, 0.74, 0.02, W)
    g.rr(0.29, 0.74, 0.71, 0.82, 0.02, L)
    g.soft_star(0.5, 0.33, 0.08, 0.035, 5, D)


def flag(g):
    g.rr(0.22, 0.14, 0.27, 0.88, 0.02, W)
    g.circ(0.245, 0.14, 0.035, W)
    x0, y0, w, h = 0.27, 0.18, 0.52, 0.38
    for r in range(4):
        for c in range(6):
            col = W if (r + c) % 2 == 0 else D
            wave_off = 0.03 * math.sin(c / 6 * math.pi * 2)
            g.poly([(x0 + c * w / 6, y0 + r * h / 4 + wave_off), (x0 + (c + 1) * w / 6, y0 + r * h / 4 + 0.03 * math.sin((c + 1) / 6 * math.pi * 2)),
                    (x0 + (c + 1) * w / 6, y0 + (r + 1) * h / 4 + 0.03 * math.sin((c + 1) / 6 * math.pi * 2)),
                    (x0 + c * w / 6, y0 + (r + 1) * h / 4 + wave_off)], col)


def wheel(g):
    g.circ(0.5, 0.5, 0.35, W)
    g.circ(0.5, 0.5, 0.25, K)
    g.circ(0.5, 0.5, 0.21, L)
    for i in range(5):
        a = math.radians(i * 72 - 90)
        g.line([(0.5, 0.5), (0.5 + 0.2 * math.cos(a), 0.5 + 0.2 * math.sin(a))], 0.05, W)
    g.circ(0.5, 0.5, 0.07, W)
    g.circ(0.5, 0.5, 0.03, D)
    for i in range(16):
        a = math.radians(i * 22.5)
        g.circ(0.5 + 0.31 * math.cos(a), 0.5 + 0.31 * math.sin(a), 0.018, D)


def tennis(g):
    g.circ(0.5, 0.5, 0.32, W)
    g.pie(0.5, 0.5, 0.32, 20, 200, L)
    g.arc(0.14, 0.5, 0.26, -55, 55, 0.035, D)
    g.arc(0.86, 0.5, 0.26, 125, 235, 0.035, D)


def bolt(g):
    g.poly([(0.56, 0.12), (0.26, 0.54), (0.47, 0.54), (0.4, 0.88), (0.74, 0.42), (0.53, 0.42), (0.62, 0.12)], W)
    g.poly([(0.56, 0.12), (0.26, 0.54), (0.47, 0.54), (0.5, 0.42), (0.53, 0.42), (0.62, 0.12)], L)


def flame(g):
    pts = []
    for t in range(0, 361, 5):
        a = math.radians(t)
        r = 0.26
        x = 0.5 + r * math.sin(a)
        y = 0.6 - r * math.cos(a)
        if math.cos(a) > 0:   # the top narrows into a tip
            k = math.cos(a)
            x = 0.5 + (x - 0.5) * (1 - k * 0.55)
            y = 0.6 - r * k * (1 + 0.95 * k)
        pts.append((x, y))
    g.poly(pts, W)
    inner = [(0.5 + (x - 0.5) * 0.6, 0.7 + (y - 0.6) * 0.6) for x, y in pts]
    g.poly(inner, L)
    core = [(0.5 + (x - 0.5) * 0.3, 0.76 + (y - 0.6) * 0.3) for x, y in pts]
    g.poly(core, W)


def heart(g):
    g.circ(0.36, 0.4, 0.17, W)
    g.circ(0.64, 0.4, 0.17, W)
    g.poly([(0.2, 0.46), (0.8, 0.46), (0.5, 0.84)], W)
    g.poly([(0.19, 0.42), (0.5, 0.42), (0.5, 0.84)], W)
    g.poly([(0.81, 0.42), (0.5, 0.42), (0.5, 0.84)], W)
    g.ell(0.32, 0.36, 0.05, 0.035, L)


def note(g):
    g.ell(0.3, 0.72, 0.11, 0.085, W)
    g.ell(0.68, 0.64, 0.11, 0.085, W)
    g.rr(0.37, 0.24, 0.42, 0.72, 0.0, W)
    g.rr(0.75, 0.16, 0.8, 0.64, 0.0, W)
    g.poly([(0.37, 0.24), (0.8, 0.16), (0.8, 0.27), (0.37, 0.35)], W)
    g.poly([(0.37, 0.32), (0.8, 0.24), (0.8, 0.27), (0.37, 0.35)], L)


def headphones(g):
    g.arc(0.5, 0.52, 0.28, 180, 360, 0.06, W)
    for s in (-1, 1):
        g.rr(0.5 + s * 0.28 - 0.08, 0.48, 0.5 + s * 0.28 + 0.08, 0.76, 0.06, W)
        g.rr(0.5 + s * 0.28 - 0.04, 0.53, 0.5 + s * 0.28 + 0.04, 0.71, 0.03, L)


def dice(g):
    g.rr(0.2, 0.2, 0.8, 0.8, 0.12, W)
    g.rr(0.2, 0.62, 0.8, 0.8, 0.12, L)
    g.rr(0.2, 0.2, 0.8, 0.7, 0.12, W)
    for x, y in ((0.33, 0.33), (0.67, 0.33), (0.5, 0.48), (0.33, 0.63), (0.67, 0.63)):
        g.circ(x, y, 0.05, D)


def robot(g):
    g.line([(0.5, 0.2), (0.5, 0.3)], 0.025, W)
    g.circ(0.5, 0.18, 0.04, L)
    g.rr(0.24, 0.3, 0.76, 0.74, 0.1, W)
    g.rr(0.3, 0.38, 0.7, 0.58, 0.06, K)
    g.circ(0.41, 0.48, 0.04, L); g.circ(0.59, 0.48, 0.04, L)
    for x in (0.4, 0.47, 0.54, 0.61):
        g.rr(x - 0.02, 0.63, x + 0.02, 0.68, 0.01, D)
    g.rr(0.18, 0.44, 0.24, 0.6, 0.02, L)
    g.rr(0.76, 0.44, 0.82, 0.6, 0.02, L)


def anchor(g):
    g.ring(0.5, 0.22, 0.06, 0.035, W)
    g.rr(0.47, 0.28, 0.53, 0.8, 0.0, W)
    g.rr(0.36, 0.34, 0.64, 0.39, 0.02, W)
    g.arc(0.5, 0.56, 0.26, 20, 160, 0.06, W)
    g.poly([(0.7, 0.64), (0.82, 0.58), (0.78, 0.72)], W)
    g.poly([(0.3, 0.64), (0.18, 0.58), (0.22, 0.72)], W)


def paw(g):
    g.ell(0.5, 0.64, 0.19, 0.16, W)
    for x, y in ((0.27, 0.44), (0.4, 0.3), (0.6, 0.3), (0.73, 0.44)):
        g.ell(x, y, 0.075, 0.09, W)
    g.ell(0.46, 0.6, 0.05, 0.035, L)


def dog(g):
    g.ell(0.5, 0.52, 0.25, 0.27, W)
    for s in (-1, 1):
        g.poly([(0.5 + s * 0.18, 0.28), (0.5 + s * 0.33, 0.36), (0.5 + s * 0.3, 0.6), (0.5 + s * 0.2, 0.52)], L)
    eyes(g, 0.48, 0.1, 0.032)
    g.ell(0.5, 0.64, 0.12, 0.09, L)
    g.ell(0.5, 0.6, 0.05, 0.035, K)
    g.rr(0.47, 0.67, 0.53, 0.76, 0.03, D)


def mouse(g):
    for s in (-1, 1):
        g.circ(0.5 + s * 0.2, 0.32, 0.14, W)
        g.circ(0.5 + s * 0.2, 0.32, 0.085, L)
    g.ell(0.5, 0.58, 0.22, 0.22, W)
    eyes(g, 0.55, 0.08, 0.028)
    g.circ(0.5, 0.65, 0.03, K)
    for s in (-1, 1):
        g.line([(0.5 + s * 0.1, 0.66), (0.5 + s * 0.25, 0.63)], 0.01, L)
        g.line([(0.5 + s * 0.1, 0.69), (0.5 + s * 0.24, 0.71)], 0.01, L)


def turtle(g):
    g.circ(0.8, 0.56, 0.08, W)
    g.circ(0.82, 0.54, 0.015, K)
    for x in (0.32, 0.62):
        g.ell(x, 0.7, 0.06, 0.05, W)
    g.pie(0.48, 0.62, 0.3, 180, 360, W)
    g.rr(0.16, 0.6, 0.8, 0.66, 0.03, L)
    for x, y in ((0.36, 0.48), (0.6, 0.48), (0.48, 0.4)):
        g.star(x, y, 0.065, 0.065, 6, D, rot=0)


def whale(g):
    g.pie(0.46, 0.5, 0.3, 0, 180, L)
    g.ell(0.46, 0.5, 0.3, 0.2, W)
    g.poly([(0.72, 0.52), (0.88, 0.38), (0.9, 0.5), (0.86, 0.62)], W)
    g.circ(0.3, 0.5, 0.03, K)
    g.arc(0.3, 0.56, 0.06, 20, 120, 0.014, D)
    for dx in (-0.05, 0.0, 0.05):
        g.line([(0.44, 0.28), (0.44 + dx * 1.6, 0.16)], 0.025, L)


def apple(g):
    g.circ(0.4, 0.56, 0.21, W)
    g.circ(0.6, 0.56, 0.21, W)
    g.ell(0.5, 0.68, 0.2, 0.16, W)
    g.line([(0.5, 0.38), (0.53, 0.24)], 0.03, D)
    g.ell(0.62, 0.27, 0.08, 0.04, L)
    g.ell(0.36, 0.48, 0.05, 0.07, L)


def cherry(g):
    g.line([(0.34, 0.62), (0.5, 0.28), (0.58, 0.2)], 0.025, L, caps=False)
    g.line([(0.66, 0.66), (0.54, 0.3)], 0.025, L, caps=False)
    g.ell(0.68, 0.22, 0.1, 0.05, W)
    g.circ(0.34, 0.66, 0.15, W)
    g.circ(0.66, 0.7, 0.15, W)
    g.circ(0.29, 0.61, 0.035, L)
    g.circ(0.61, 0.65, 0.035, L)


def icecream(g):
    g.poly([(0.32, 0.5), (0.68, 0.5), (0.5, 0.9)], L)
    for i in range(1, 4):
        g.line([(0.32 + i * 0.09, 0.5), (0.5 - 0.06 + i * 0.02, 0.86)], 0.012, D, caps=False)
    g.circ(0.5, 0.38, 0.16, W)
    g.circ(0.36, 0.48, 0.07, W)
    g.circ(0.5, 0.5, 0.07, W)
    g.circ(0.64, 0.48, 0.07, W)
    g.circ(0.55, 0.2, 0.045, D)
    g.circ(0.44, 0.32, 0.035, L)


def donut(g):
    g.circ(0.5, 0.5, 0.33, L)
    g.d.ellipse([0.19 * N, 0.17 * N, 0.81 * N, 0.79 * N], fill=W)
    g.circ(0.5, 0.48, 0.1, CLEAR)
    g.ring(0.5, 0.48, 0.1, 0.02, L)
    import random
    rnd = random.Random(7)
    for _ in range(14):
        a = rnd.uniform(0, math.tau)
        r = rnd.uniform(0.16, 0.26)
        x, y = 0.5 + r * math.cos(a), 0.48 + r * math.sin(a)
        b = rnd.uniform(0, math.tau)
        g.line([(x, y), (x + 0.03 * math.cos(b), y + 0.03 * math.sin(b))], 0.018, D if _ % 2 else L)


def cake(g):
    g.poly([(0.18, 0.46), (0.82, 0.36), (0.82, 0.76), (0.18, 0.82)], W)
    g.poly([(0.18, 0.58), (0.82, 0.5), (0.82, 0.56), (0.18, 0.64)], D)
    g.poly([(0.18, 0.46), (0.82, 0.36), (0.82, 0.42), (0.18, 0.52)], L)
    g.circ(0.62, 0.32, 0.07, W)
    g.line([(0.62, 0.26), (0.66, 0.18)], 0.015, L)
    g.circ(0.6, 0.3, 0.02, L)


def pizza(g):
    g.poly([(0.2, 0.26), (0.8, 0.26), (0.5, 0.86)], W)
    g.rr(0.16, 0.18, 0.84, 0.3, 0.06, L)
    for x, y in ((0.42, 0.42), (0.6, 0.44), (0.5, 0.62)):
        g.circ(x, y, 0.05, D)
    g.ell(0.38, 0.55, 0.02, 0.04, L)


def coffee(g):
    g.rr(0.24, 0.4, 0.66, 0.8, 0.08, W)
    g.ring(0.71, 0.58, 0.09, 0.04, W)
    g.rr(0.28, 0.44, 0.62, 0.5, 0.02, D)
    g.ell(0.45, 0.84, 0.28, 0.03, L)
    for x in (0.36, 0.46, 0.56):
        g.line([(x, 0.34), (x - 0.025, 0.28), (x, 0.22), (x - 0.02, 0.16)], 0.02, L)


def cupcake(g):
    g.poly([(0.3, 0.54), (0.7, 0.54), (0.64, 0.84), (0.36, 0.84)], L)
    for x in (0.4, 0.5, 0.6):
        g.line([(x, 0.56), (x - (x - 0.5) * 0.25, 0.82)], 0.014, D, caps=False)
    g.circ(0.5, 0.44, 0.16, W)
    g.circ(0.34, 0.52, 0.08, W)
    g.circ(0.66, 0.52, 0.08, W)
    g.circ(0.5, 0.25, 0.05, D)
    for x, y in ((0.42, 0.42), (0.58, 0.46), (0.5, 0.52)):
        g.circ(x, y, 0.014, D)


ICONS = [
    ("SteelTemple", [("Sword", sword), ("Shield", shield), ("Crown", crown), ("Gem", gem), ("Key", key),
                     ("Potion", potion), ("Temple", temple), ("Compass", compass)]),
    ("Creatures", [("Cat", cat), ("Fox", fox), ("Owl", owl), ("Bear", bear), ("Bunny", bunny), ("Frog", frog),
                   ("Penguin", penguin), ("Panda", panda), ("Bird", bird), ("Fish", fish), ("Octopus", octopus),
                   ("Ghost", ghost), ("Dog", dog), ("Mouse", mouse), ("Turtle", turtle), ("Whale", whale)]),
    ("Food", [("Apple", apple), ("Cherries", cherry), ("Ice cream", icecream), ("Donut", donut), ("Cake", cake),
              ("Pizza", pizza), ("Coffee", coffee), ("Cupcake", cupcake)]),
    ("Nature", [("Leaf", leaf), ("Flower", flower), ("Tree", tree), ("Mountain", mountain), ("Wave", wave),
                ("Sun", sun), ("Moon", moon), ("Snowflake", snowflake)]),
    ("Space", [("Star", star), ("Planet", planet), ("Rocket", rocket), ("Comet", comet), ("UFO", ufo),
               ("Alien", alien)]),
    ("Play", [("Gamepad", gamepad), ("Trophy", trophy), ("Flag", flag), ("Wheel", wheel), ("Tennis", tennis),
              ("Bolt", bolt), ("Flame", flame), ("Heart", heart), ("Note", note), ("Headphones", headphones),
              ("Dice", dice), ("Robot", robot), ("Anchor", anchor), ("Paw", paw)]),
]


def render(fn):
    g = Pen()
    fn(g)
    art = g.im
    # a soft shadow under the shape lifts it off the background
    alpha = art.getchannel("A").point(lambda v: 255 if v > 0 else 0)
    shadow = Image.new("RGBA", art.size, (0, 0, 0, 0))
    shadow.putalpha(alpha.point(lambda v: v * 60 // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(N * 0.018))
    out = Image.new("RGBA", art.size, CLEAR)
    out.alpha_composite(shadow, (0, int(N * 0.018)))
    out.alpha_composite(art)
    return out.resize((CELL, CELL), Image.LANCZOS)


def main():
    root = Path(__file__).resolve().parent.parent
    flat = [(cat, name, fn) for cat, items in ICONS for name, fn in items]
    rows = (len(flat) + COLS - 1) // COLS
    atlas = Image.new("RGBA", (COLS * CELL, rows * CELL), CLEAR)
    cells = []
    for i, (cat, name, fn) in enumerate(flat):
        im = render(fn)
        cells.append(im)
        atlas.alpha_composite(im, ((i % COLS) * CELL, (i // COLS) * CELL))
    atlas.save(root / "assets" / "avatars.png", optimize=True)
    with open(root / "assets" / "avatars.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# cell {CELL} columns {COLS}: category, name (tools/make_avatars.py)\n")
        for cat, name, _ in flat:
            f.write(f"{cat}\t{name}\n")
    print(f"{len(flat)} icons -> assets/avatars.png ({atlas.size[0]}x{atlas.size[1]})")

    if "--sheet" in sys.argv:   # every icon on sample colours, for checking by eye
        colours = [(232, 36, 58), (255, 128, 32), (40, 170, 90), (30, 150, 200), (110, 70, 220), (230, 70, 150)]
        sheet = Image.new("RGBA", (COLS * CELL, rows * CELL), (30, 30, 36, 255))
        for i, im in enumerate(cells):
            x, y = (i % COLS) * CELL, (i // COLS) * CELL
            bg = Image.new("RGBA", (CELL, CELL), CLEAR)
            ImageDraw.Draw(bg).ellipse([6, 6, CELL - 6, CELL - 6], fill=colours[i % len(colours)] + (255,))
            bg.alpha_composite(im)
            sheet.alpha_composite(bg, (x, y))
        sheet.save(sys.argv[sys.argv.index("--sheet") + 1])


if __name__ == "__main__":
    main()
