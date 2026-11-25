# Generates the three gate concepts of documents/built_3d.md (#618) as archetypes: python tools/gen_gate_concepts.py out.json
import json, math, sys
AMBER = [236, 170, 60]
LIGHT = [255, 214, 150]
HOT = [255, 226, 170]


def rnd(v):
    return round(v, 4)


def P(form, **k):
    d = {"form": form}
    d["role"] = k.pop("role", "hull")
    d["z"] = k.pop("z", -3)
    for key, v in k.items():
        d[key] = v
    return d


def bar(at, l, w, h, angle=0, role="hull", z=-3, **k):
    return P("bar", at=[rnd(at[0]), rnd(at[1])], length=l, width=w, height=h, angle=angle, role=role, z=z, **k)


def disc(at, r, h, role="hull", z=-3, **k):
    return P("disc", at=[rnd(at[0]), rnd(at[1])], radius=r, height=h, role=role, z=z, **k)


def lamp(at, r, lift=None, th=0.025, tint=AMBER, blink=0, alpha=1.0, z=-1, **k):
    d = P("disc", at=[rnd(at[0]), rnd(at[1])], radius=r, role="light", z=z, tint=tint, height=th, **k)
    if lift is not None:
        d["lift"] = lift
    if blink:
        d["blink"] = blink
    if alpha < 1:
        d["alpha"] = alpha
    return d


def lane(chev=True):
    out = []
    # approach lights: a funnel opening away from the aperture
    out.append(lamp([-0.30, 0.10], 0.016, th=0.02, blink=1.4, mirror=True, row={"count": 7, "step": [-0.18, 0.05]}))
    for x, y in ((-0.66, 0.20), (-1.20, 0.35)):
        out.append(disc([x, y], 0.03, 0.10, mirror=True))
        out.append(lamp([x, y], 0.022, lift=0.10, th=0.03, blink=1.4, mirror=True))
    if chev:
        out.append(P("chevron", at=[-0.45, 0.0], length=0.15, width=0.24, tip=0.0, angle=0, role="trim", tint=AMBER,
                     height=0.008, z=-3, row={"count": 3, "step": [-0.27, 0.0]}))
    return out


def swirl(r, at=(0, 0)):
    return {"form": "disc", "at": list(at), "radius": r, "role": "light", "soft": True, "material": "wormhole", "z": -3}


# ---------------- Concept A: the doorway ----------------
def a_jump():
    a = 0.46
    p = []
    p.append(bar([0, a], 0.46, 0.30, 0.10, mirror=True))
    p.append(bar([0, a], 0.24, 0.16, 0.80, z=-2, mirror=True))
    p.append(bar([0, a], 0.16, 0.10, 0.03, z=-1, role="trim", tint=AMBER, mirror=True))
    p.append(bar([0, 0], 0.92, 0.20, 0.10, angle=90, lift=0.76, z=-1))
    p.append(bar([0, 0], 0.88, 0.08, 0.02, angle=90, lift=0.86, z=0, role="trim", tint=AMBER))
    p.append(bar([0, 0], 0.84, 0.014, 0.74, angle=90, lift=0.02, role="light", alpha=0.8, tint=LIGHT, z=-2))
    p.append(bar([0, 0], 0.9, 0.07, 0.02, angle=90, role="trim", tint=AMBER, z=-2))
    p += lane()
    return p


def a_anchor():
    p = [swirl(0.62)]
    for k in range(4):
        deg = 45 + 90 * k
        ang = math.radians(deg)
        c, s = math.cos(ang), math.sin(ang)

        def at(r):
            return [r * c, r * s]

        p.append(bar(at(0.76), 0.34, 0.30, 0.10, angle=deg))
        p.append(bar(at(0.76), 0.20, 0.20, 0.46, angle=deg, z=-2))
        p.append(bar(at(0.50), 0.34, 0.09, 0.07, angle=deg, lift=0.42, z=-1))
        p.append(lamp(at(0.34), 0.03, lift=0.40, th=0.04, tint=HOT, z=0))
        p.append(bar(at(0.76), 0.12, 0.12, 0.03, angle=deg, z=-1, role="trim", tint=AMBER))
    p.append(P("ring", radius=0.70, width=0.04, height=0.02, role="trim", tint=AMBER, z=-3))
    p += lane()
    return p


# ---------------- Concept B: the hoops ----------------
def b_jump():
    p = []
    N = 6
    for k in range(N):
        x = -0.9 + 0.38 * k
        a = 0.52 - 0.055 * k
        H = 0.62 - 0.07 * k
        pw = 0.13 if k == 0 else 0.09
        p.append(bar([x, a], pw + 0.04, pw + 0.04, 0.06, mirror=True))
        p.append(bar([x, a], pw, pw, H, z=-2, mirror=True))
        p.append(bar([x, 0], 2 * a + pw, pw * 0.8, 0.07, angle=90, lift=H, z=-1))
        p.append(lamp([x, a - pw * 0.8], 0.022, lift=H * 0.55, th=0.05, tint=HOT, z=0, mirror=True))
        p.append(bar([x, 0], 2 * a, 0.012, H - 0.02, angle=90, lift=0.02, role="light", alpha=0.34 + 0.06 * k,
                     tint=LIGHT, z=-2))
    p.append(bar([0.05, 0], 2.2, 0.05, 0.015, role="trim", tint=AMBER, z=-3))
    p.append(P("chevron", at=[-1.0, 0.0], length=0.14, width=0.2, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.008, z=-3, row={"count": 8, "step": [0.26, 0.0]}))
    p.append(lamp([-1.25, 0.16], 0.016, th=0.02, blink=1.4, mirror=True, row={"count": 4, "step": [-0.16, 0.04]}))
    p.append(bar([1.0, 0], 1.0, 0.02, 0.5, angle=90, lift=0.02, role="light", alpha=0.5, tint=HOT, z=-2))
    return p


def b_anchor():
    p = [swirl(0.5, (0.05, 0))]
    for x in (-0.55, 0.1, 0.75):
        a = 0.66
        p.append(bar([x, a], 0.32, 0.24, 0.08, mirror=True))
        p.append(bar([x, a], 0.16, 0.16, 0.50, z=-2, mirror=True))
        p.append(bar([x, 0], 2 * a + 0.16, 0.14, 0.08, angle=90, lift=0.48, z=-1))
        p.append(lamp([x, a - 0.12], 0.026, lift=0.28, th=0.05, tint=HOT, z=0, mirror=True))
    p.append(bar([0.1, 0], 2.2, 0.05, 0.015, role="trim", tint=AMBER, z=-3))
    p.append(P("chevron", at=[-1.0, 0.0], length=0.14, width=0.2, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.008, z=-3, row={"count": 3, "step": [-0.27, 0.0]}))
    p.append(lamp([-1.2, 0.16], 0.016, th=0.02, blink=1.4, mirror=True, row={"count": 4, "step": [-0.16, 0.04]}))
    return p


# ---------------- Concept C: the crown ----------------
def c_jump():
    p = []
    for g in (-150, -100, -50, 0, 50, 100, 150):
        ar = math.radians(g)
        c, s = math.cos(ar), math.sin(ar)

        def at(r):
            return [r * c, r * s]

        p.append(disc(at(0.80), 0.075, 0.09))
        p.append(disc(at(0.80), 0.04, 0.46, z=-2))
        p.append(lamp(at(0.80), 0.036, lift=0.55, th=0.04, tint=AMBER, z=0))
        for r, l in ((0.64, 0.38), (0.48, 0.26), (0.32, 0.14)):
            p.append(lamp(at(r), 0.014, lift=l, th=0.018, tint=LIGHT, z=0))
    p.append(P("arc", radius=0.84, width=0.07, height=0.03, role="hull", z=-3, **{"from": -150, "to": 150}))
    p.append(lamp([0, 0], 0.28, lift=0.005, th=0.012, tint=LIGHT, alpha=0.7, z=-2))
    p.append(lamp([0, 0], 0.10, lift=0.02, th=0.012, tint=HOT, alpha=0.85, z=-1))
    p.append(bar([1.22, 0], 0.5, 0.07, 0.04))
    p.append(P("chevron", at=[1.02, 0.0], length=0.12, width=0.16, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.01, z=-3, row={"count": 3, "step": [0.18, 0.0]}))
    p += lane()
    return p


def c_anchor():
    p = [swirl(0.58)]
    for g in (-144, -72, 0, 72, 144):
        ar = math.radians(g)
        c, s = math.cos(ar), math.sin(ar)

        def at(r):
            return [r * c, r * s]

        p.append(disc(at(0.86), 0.12, 0.16))
        p.append(disc(at(0.86), 0.065, 0.44, z=-2))
        p.append(bar(at(0.62), 0.40, 0.08, 0.07, angle=g, lift=0.50, z=-1))
        p.append(lamp(at(0.42), 0.03, lift=0.48, th=0.04, tint=HOT, z=0))
    p.append(P("arc", radius=0.9, width=0.08, height=0.04, role="hull", z=-3, **{"from": -150, "to": 150}))
    p.append(P("ring", radius=0.7, width=0.03, height=0.02, role="trim", tint=AMBER, z=-3))
    p += lane()
    return p


# ---------------- Pass 3: the tunnel, with a field of its own (D) ----------------
# The field is a soft disc of the `gatefield` material (data/shaders/materials/gatefield.fs); the
# hoops stand where its `rungs` say. Disc centre FX, radius FR: a hoop at local x = RUNG0 + k *
# STEP is at FX + FR * (RUNG0 + k * STEP).
FX, FR = 0.15, 1.05
RUNG0, STEP, RUNGS = -0.62, 0.34, 4


def rung_x(k):
    return FX + FR * (RUNG0 + STEP * k)


def field(material, r, at=(0, 0), angle=0):
    return [
        {"form": "disc", "at": [rnd(at[0]), rnd(at[1])], "radius": r, "role": "light", "soft": True,
         "material": material, "z": -3},
        {"form": "disc", "at": [rnd(at[0]), rnd(at[1])], "radius": r * 0.7, "role": "light", "soft": True,
         "alpha": 0.5, "tint": HOT, "fallback": True, "z": -3},
    ]


def tower(x, y, w, d, hb, hs, hc=0.05):
    """A stepped tower: plate, shaft, cap with an amber line."""
    p = [bar([x, y], w, d, hb)]
    p.append(bar([x, y], w * 0.72, d * 0.72, hs, z=-2))
    # The cap: an amber line across the top, not a slab of it.
    p.append(bar([x, y], w * 0.86, d * 0.10, hc, z=-1, role="trim", tint=AMBER))
    p.append(bar([x, y], w * 0.10, d * 0.86, hc, z=-1, role="trim", tint=AMBER))
    return p


def d_jump():
    p = []
    # The entrance: A's tower mass and a heavy lintel, the face of the gate.
    ex = -0.98
    for sgn in (1, -1):
        p += tower(ex, sgn * 0.54, 0.50, 0.40, 0.08, 0.78)
        p.append(lamp([ex + 0.20, sgn * 0.54 * 0.86], 0.03, lift=0.62, th=0.05, tint=HOT, z=0))
    p.append(bar([ex, 0], 1.30, 0.26, 0.11, angle=90, lift=0.74, z=-1))
    p.append(bar([ex, 0], 1.20, 0.10, 0.025, angle=90, lift=0.88, z=0, role="trim", tint=AMBER))
    p.append(lamp([ex + 0.1, 0.0], 0.04, lift=0.70, th=0.03, tint=HOT, z=0, mirror=True, row={"count": 3, "step": [0.0, 0.16]}))
    # The hoops, each smaller and lower than the last, standing where the field's rungs are.
    prev = None
    for k in range(RUNGS):
        x = rung_x(k)
        a = 0.56 - 0.04 * k
        H = 0.50 - 0.05 * k
        pw = 0.17 - 0.012 * k
        p.append(bar([x, a], pw + 0.10, 0.20, 0.07, mirror=True, role="panel"))
        p.append(bar([x, a], 0.085, 0.085, H, z=-2, mirror=True))
        p.append(bar([x, 0], 2 * a + 0.085, 0.06, 0.07, angle=90, lift=H, z=-1))
        # The hoop's own sheet of light, thin, there so a hoop is a portal and not a frame.
        p.append(bar([x, 0], 2 * a - pw, 0.012, H - 0.02, angle=90, lift=0.02, role="light", alpha=0.30 + 0.05 * k,
                     tint=LIGHT, z=-2))
        p.append(bar([x, 0], 2 * a * 0.9, 0.025, 0.02, angle=90, lift=H + 0.07, z=0, role="trim", tint=AMBER))
        p.append(lamp([x, a - pw * 0.8], 0.026, lift=H * 0.55, th=0.05, tint=HOT, z=0, mirror=True))
        # A roof rail from this hoop to the next, so the tunnel reads as one long frame.
        if k + 1 < RUNGS:
            x2 = rung_x(k + 1)
            for sgn in (1, -1):
                p.append(bar([(x + x2) / 2, sgn * a * 0.97], x2 - x, 0.04, 0.025, lift=H - 0.01, z=-1))
    # The first rail runs from the entrance to the first hoop.
    for sgn in (1, -1):
        p.append(bar([(ex + 0.25 + rung_x(0)) / 2, sgn * 0.57], rung_x(0) - ex - 0.25, 0.05, 0.03, lift=0.60, z=-1))
        # The hold line on the ground, along the lane's edge.
        p.append(bar([0.05, sgn * 0.44], 2.0, 0.025, 0.022, role="trim", tint=AMBER, z=-3))
    # The exit: emitter posts and a small crown round the core.
    for sgn in (1, -1):
        p.append(disc([0.93, sgn * 0.40], 0.065, 0.30, role="trim"))
        p.append(lamp([0.93, sgn * 0.40], 0.04, lift=0.32, th=0.04, tint=HOT, z=0))
        p.append(disc([1.12, sgn * 0.20], 0.045, 0.22, role="trim"))
        p.append(lamp([1.12, sgn * 0.20], 0.03, lift=0.24, th=0.04, tint=AMBER, z=0))
    p.append(bar([1.34, 0], 0.30, 0.06, 0.05))
    p.append(P("chevron", at=[1.14, 0.0], length=0.1, width=0.14, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.008, z=-3, row={"count": 2, "step": [0.14, 0.0]}))
    # The approach: chevrons on the ground and a funnel of blinking lights running toward it.
    p.append(P("chevron", at=[-1.45, 0.0], length=0.15, width=0.24, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.008, z=-3, row={"count": 4, "step": [-0.27, 0.0]}))
    p.append(lamp([-1.40, 0.16], 0.016, th=0.02, blink=1.4, mirror=True, row={"count": 6, "step": [-0.18, 0.05]}))
    p += field("gatefield", FR, (FX, 0))
    return p


def d_anchor():
    p = field("gatefield.anchor", 0.60)
    # Four heavy clamps on the diagonals, each a tower and a claw reaching over the field.
    for k in range(4):
        deg = 45 + 90 * k
        ang = math.radians(deg)
        c, s = math.cos(ang), math.sin(ang)

        def at(r):
            return [r * c, r * s]

        p.append(bar(at(0.82), 0.40, 0.34, 0.12, angle=deg))
        p.append(bar(at(0.82), 0.30, 0.26, 0.52, angle=deg, z=-2))
        p.append(bar(at(0.82), 0.30, 0.035, 0.02, angle=deg, z=-1, role="trim", tint=AMBER))
        p.append(bar(at(0.56), 0.46, 0.10, 0.08, angle=deg, lift=0.50, z=-1))
        p.append(bar(at(0.56), 0.40, 0.04, 0.02, angle=deg, lift=0.58, z=0, role="trim", tint=AMBER))
        p.append(lamp(at(0.36), 0.032, lift=0.48, th=0.045, tint=HOT, z=0))
        p.append(lamp(at(0.84), 0.03, lift=0.56, th=0.04, tint=AMBER, z=0))
    p.append(P("ring", radius=0.70, width=0.045, height=0.02, role="trim", tint=AMBER, z=-3))
    p.append(P("ring", radius=0.95, width=0.06, height=0.03, role="hull", z=-3))
    p.append(lamp([0.66, 0.0], 0.02, th=0.02, z=-1, tint=HOT, blink=2.4, row={"count": 1, "step": [0, 0]}))
    # The approach, as the jump gate's.
    p.append(P("chevron", at=[-1.25, 0.0], length=0.15, width=0.24, tip=0.0, angle=0, role="trim", tint=AMBER,
               height=0.008, z=-3, row={"count": 3, "step": [-0.27, 0.0]}))
    p.append(lamp([-1.2, 0.16], 0.016, th=0.02, blink=1.4, mirror=True, row={"count": 5, "step": [-0.18, 0.05]}))
    return p


def arch(id_, name, parts, size=900):
    return {"id": id_, "world": {"category": "gates"}, "name": name, "kind": "Gate", "glyph": "H", "sprite": "gate",
            "color": [168, 156, 142, 255], "layer": 3, "size": size, "material": "plating", "livery": "relay",
            "shape": {"parts": parts}, "components": {"jumpLink": {"range": 200}}}


out = []
for tag, j, a in (("a", a_jump, a_anchor), ("b", b_jump, b_anchor), ("c", c_jump, c_anchor), ("d", d_jump, d_anchor)):
    out.append(arch("gate.%s.jump" % tag, "Jump Gate", j()))
    out.append(arch("gate.%s.anchor" % tag, "Anchor Gate", a()))
json.dump(out, open(sys.argv[1], "w"), indent=1)
