import math, random, itertools

def gen(W, H, seed, box, dmin, dmax, dots=True):
    random.seed(seed)
    CX, CY, ANG = W / 2, H / 2, -16
    STREET, AVENUE = 9, 20
    diag = math.hypot(W, H)
    xmin, xmax = CX - diag / 2 - 120, CX + diag / 2 + 120
    ymin, ymax = CY - diag / 2 - 120, CY + diag / 2 + 120
    def seq(lo, hi, sizes, every):
        edges, x, i = [], lo, 0
        while x < hi:
            w = random.choice(sizes); edges.append((x, x + w))
            x += w + (AVENUE if i % every == every - 1 else STREET); i += 1
        return edges
    cols = seq(xmin, xmax, [46, 58, 70, 88, 104], 8)
    rows = seq(ymin, ymax, [34, 44, 56, 72], 9)
    a = math.radians(ANG)
    def ts(x, y):
        dx, dy = x - CX, y - CY
        return (CX + dx * math.cos(a) - dy * math.sin(a), CY + dx * math.sin(a) + dy * math.cos(a))
    out = [f'<g transform="rotate({ANG} {CX:g} {CY:g})">']
    for (x0, x1) in cols:
        for (y0, y1) in rows:
            p = [ts(x, y) for x in (x0, x1) for y in (y0, y1)]
            if max(q[0] for q in p) < -10 or min(q[0] for q in p) > W + 10: continue
            if max(q[1] for q in p) < -10 or min(q[1] for q in p) > H + 10: continue
            r = random.random(); w, h = x1 - x0, y1 - y0
            parts = []
            if r < 0.08: parts = [('lot', x0, y0, w, h)]
            elif r < 0.34 and h >= 44:
                s = round(h * random.choice([0.4, 0.55]))
                parts = [('b', x0, y0, w, s - 2), ('b', x0, y0 + s + 2, w, h - s - 2)]
            elif r < 0.5 and w >= 70:
                s = round(w * random.choice([0.45, 0.6]))
                parts = [('b', x0, y0, s - 2, h), ('b', x0 + s + 2, y0, w - s - 2, h)]
            else:
                parts = [('b2' if random.random() < 0.16 else 'b', x0, y0, w, h)]
            for k, x, y, ww, hh in parts:
                if k == 'lot':
                    out.append(f'<rect x="{x:g}" y="{y:g}" width="{ww:g}" height="{hh:g}" rx="2" fill="none" stroke="#252B33" stroke-width="1"></rect>')
                else:
                    out.append(f'<rect x="{x:g}" y="{y:g}" width="{ww:g}" height="{hh:g}" rx="2" fill="{"#20262E" if k == "b2" else "#1B2027"}"></rect>')
    if dots:
        vs = [(cols[i][1] + cols[i + 1][0]) / 2 for i in range(len(cols) - 1)]
        hs = [(rows[i][1] + rows[i + 1][0]) / 2 for i in range(len(rows) - 1)]
        bx0, by0, bx1, by1 = box
        inb = lambda x, y: bx0 <= x <= bx1 and by0 <= y <= by1
        pts = [(i, j, *ts(vx, hy)) for i, vx in enumerate(vs) for j, hy in enumerate(hs)]
        pts = [p for p in pts if inb(p[2], p[3])]
        best = None
        for (i1, j1, x1, y1), (i2, j2, x2, y2) in itertools.permutations(pts, 2):
            if i1 == i2 or j1 == j2 or x2 <= x1: continue
            c = ts(vs[i1], hs[j2])
            if not inb(*c): continue
            d = math.hypot(x2 - x1, y2 - y1)
            if dmin <= d <= dmax:
                score = abs(d - (dmin + dmax) / 2)
                if best is None or score < best[0]: best = (score, i1, j1, i2, j2)
        assert best, 'no route'
        _, i1, j1, i2, j2 = best
        ax, ay, bx, by = vs[i1], hs[j1], vs[i2], hs[j2]
        out.append(f'<path d="M{ax:g} {ay:g} L{ax:g} {by:g} L{bx:g} {by:g}" fill="none" stroke="#5B6573" stroke-width="2" stroke-dasharray="2 5" stroke-linecap="round"></path>')
        out.append(f'<circle cx="{ax:g}" cy="{ay:g}" r="13" fill="#F4F4EF" opacity="0.10"></circle>')
        out.append(f'<circle cx="{ax:g}" cy="{ay:g}" r="4.5" fill="#F4F4EF"></circle>')
        out.append(f'<circle cx="{bx:g}" cy="{by:g}" r="13" fill="#D20000" opacity="0.16"></circle>')
        out.append(f'<circle cx="{bx:g}" cy="{by:g}" r="4.5" fill="#D20000"></circle>')
    out.append('</g>')
    return '\n'.join(out)

if __name__ == '__main__':
    specs = {
        'hero': (1440, 716, 21, (760, 96, 1240, 176), 150, 280),
        'shot_big': (1200, 520, 33, (820, 60, 1140, 210), 140, 260),
        'shot_a': (386, 250, 44, (130, 18, 378, 160), 60, 180),
        'shot_b': (386, 250, 55, (130, 18, 378, 160), 60, 180),
        'cta': (1200, 380, 66, (800, 60, 1140, 170), 140, 260),
    }
    for n, sp in specs.items():
        s = gen(*sp)
        open(f'{n}.svgfrag', 'w').write(s)
        print(n, s.count('<rect'), len(s))
