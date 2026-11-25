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


def arch(id_, name, parts, size=900):
    return {"id": id_, "world": {"category": "gates"}, "name": name, "kind": "Gate", "glyph": "H", "sprite": "gate",
            "color": [168, 156, 142, 255], "layer": 3, "size": size, "material": "plating", "livery": "relay",
            "shape": {"parts": parts}, "components": {"jumpLink": {"range": 200}}}


out = []
for tag, j, a in (("a", a_jump, a_anchor), ("b", b_jump, b_anchor), ("c", c_jump, c_anchor)):
    out.append(arch("gate.%s.jump" % tag, "Jump Gate", j()))
    out.append(arch("gate.%s.anchor" % tag, "Anchor Gate", a()))
json.dump(out, open(sys.argv[1], "w"), indent=1)
