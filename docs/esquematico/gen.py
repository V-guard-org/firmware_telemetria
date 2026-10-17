"""Builds esquematico.html, the wiring sheets of docs/BANCADA.md drawn as inline SVG.

    python3 docs/esquematico/gen.py                    # writes docs/esquematico/esquematico.html
    python3 docs/esquematico/gen.py --fragment PATH    # the page without <html>/<head>, as published on claude.ai

The text of the page is in page.html; each {{name}} there becomes the drawing of fig_name().
Only the standard library. The page text is Portuguese, like the roteiros: it is for the team to read.
"""
import argparse
import html
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "esquematico.html")


def e(s):
    return html.escape(str(s), quote=True)


# ---------- primitives ----------

def t(x, y, s, cls="t", anchor="start"):
    return f'<text x="{x:g}" y="{y:g}" class="{cls}" text-anchor="{anchor}">{e(s)}</text>'


def pl(pts, cls, dash=False):
    p = " ".join(f"{x:g},{y:g}" for x, y in pts)
    return f'<polyline class="wire {cls}{" dash" if dash else ""}" points="{p}"/>'


def path(d, cls, dash=False):
    return f'<path class="wire {cls}{" dash" if dash else ""}" d="{d}"/>'


def dot(x, y, cls):
    return f'<circle class="dot {cls}" cx="{x:g}" cy="{y:g}" r="4"/>'


def res(x, y, v=False):
    """An IEC resistor (a box). Ends: vertical (x, y±18), horizontal (x±18, y)."""
    if v:
        return f'<rect class="comp" x="{x-7:g}" y="{y-18:g}" width="14" height="36" rx="2"/>'
    return f'<rect class="comp" x="{x-18:g}" y="{y-7:g}" width="36" height="14" rx="2"/>'


def ptc(x, y):
    return res(x, y) + f'<line class="compline" x1="{x-14:g}" y1="{y+10:g}" x2="{x+14:g}" y2="{y-10:g}"/>'


def diode_v(x, y):
    """A vertical diode: anode on top (y-10), cathode, the band, below (y+10)."""
    return (f'<polygon class="compsolid" points="{x-9:g},{y-8:g} {x+9:g},{y-8:g} {x:g},{y+8:g}"/>'
            f'<line class="compline" x1="{x-10:g}" y1="{y+8:g}" x2="{x+10:g}" y2="{y+8:g}"/>'
            f'<line class="compline" x1="{x:g}" y1="{y-10:g}" x2="{x:g}" y2="{y-8:g}"/>'
            f'<line class="compline" x1="{x:g}" y1="{y+8:g}" x2="{x:g}" y2="{y+10:g}"/>')


def led_v(x, y):
    s = diode_v(x, y)
    for dy in (-4, 4):
        x1, y1, x2, y2 = x + 11, y + dy, x + 19, y + dy - 8
        s += f'<line class="compline" x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}"/>'
        s += f'<polygon class="compsolid" points="{x2:g},{y2:g} {x2-5:g},{y2+1:g} {x2-1:g},{y2+5:g}"/>'
    return s


def gnd(x, y):
    """A ground symbol hanging from (x, y)."""
    return (f'<line class="compline" x1="{x:g}" y1="{y:g}" x2="{x:g}" y2="{y+6:g}"/>'
            f'<line class="compline" x1="{x-10:g}" y1="{y+6:g}" x2="{x+10:g}" y2="{y+6:g}"/>'
            f'<line class="compline" x1="{x-6:g}" y1="{y+10:g}" x2="{x+6:g}" y2="{y+10:g}"/>'
            f'<line class="compline" x1="{x-2:g}" y1="{y+14:g}" x2="{x+2:g}" y2="{y+14:g}"/>')


def flag(x, y, label, cls):
    """A supply bar the wire hangs from. The same name is the same net."""
    return (f'<line class="wire {cls}" x1="{x-10:g}" y1="{y:g}" x2="{x+10:g}" y2="{y:g}"/>'
            + t(x, y - 7, label, f"tl f{cls[1:]}", "middle"))


def tag(x, y, label, cls):
    """A net label at the end of a horizontal wire, pointing right."""
    w = 12 + 7.4 * len(label)
    pts = f"{x:g},{y-9:g} {x+w:g},{y-9:g} {x+w+8:g},{y:g} {x+w:g},{y+9:g} {x:g},{y+9:g}"
    return f'<polygon class="tagbox {cls}" points="{pts}"/>' + t(x + 6, y + 4, label, f"tl f{cls[1:]}")


def jumper(x, y, label):
    return (f'<rect class="jcap" x="{x-18:g}" y="{y-9:g}" width="36" height="18" rx="4"/>'
            f'<circle class="pinhole" cx="{x-9:g}" cy="{y:g}" r="3.6"/>'
            f'<circle class="pinhole" cx="{x+9:g}" cy="{y:g}" r="3.6"/>'
            + t(x, y - 15, label, "tl", "middle"))


def block(x, y, w, h, dashed=False):
    return f'<rect class="block{" dashed" if dashed else ""}" x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}" rx="6"/>'


def pin(x, y, side, label, cls="t"):
    s = f'<rect class="pinsq" x="{x-4:g}" y="{y-4:g}" width="8" height="8"/>'
    if side == "r":
        s += t(x - 10, y + 4, label, cls, "end")
    elif side == "l":
        s += t(x + 10, y + 4, label, cls, "start")
    elif side == "t":
        s += t(x, y + 20, label, cls, "middle")
    elif side == "b":
        s += t(x, y - 11, label, cls, "middle")
    return s


def arrow(x, y, d, cls):
    return f'<polygon class="arrow {cls}" points="{x-6*d:g},{y-5:g} {x+6*d:g},{y:g} {x-6*d:g},{y+5:g}"/>'


def mark(x, y, letter):
    return f'<circle class="markc" cx="{x:g}" cy="{y:g}" r="10"/>' + t(x, y + 4, letter, "markt", "middle")


def num(x, y, n, cls):
    return f'<circle class="numc {cls}" cx="{x:g}" cy="{y:g}" r="8.5"/>' + t(x, y + 3.4, n, "numt", "middle")


def svg(w, h, body, label, minw):
    return (f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="{e(label)}" '
            f'style="min-width:{minw}px;max-width:{w}px">{"".join(body)}</svg>')


# ---------- sheet 1: the phase-2 desk ----------

def fig_desk():
    b = []
    b.append(block(20, 115, 190, 100))
    b += [t(34, 140, "NOTEBOOK", "tb"), t(34, 160, "simulator.py", "ts"), t(34, 176, "slcand: adaptador", "ts"),
          t(34, 192, "vira o can0", "ts")]
    b.append(pl([(210, 165), (270, 165)], "cusb"))
    b.append(t(240, 157, "USB", "ts", "middle"))
    b.append(block(270, 50, 110, 240))
    b += [t(284, 76, "HUB USB", "tb"), t(284, 94, "portas com", "ts"), t(284, 110, "etiqueta", "ts")]
    rows = [
        (85, "1", "PLACA DO FIRMWARE", "/dev/vguard-esp32", "programa vguard",
         [("c33", "3V3"), ("c27", "GPIO27"), ("ctx", "TX 25"), ("crx", "RX 26"), ("cgnd", "GND")]),
        (170, "2", "ADAPTADOR USB-CAN", "/dev/vguard-adaptador", "Doggie · TX 26 · RX 25",
         [("ctx", "TX 26"), ("crx", "RX 25"), ("cgnd", "GND")]),
        (255, "3", "GERADOR DE TRÁFEGO", "/dev/vguard-gerador", "tools/can-trafego",
         [("ctx", "TX 25"), ("crx", "RX 26"), ("cgnd", "GND")]),
    ]
    for cy, port, name, dev, prog, wires in rows:
        b.append(pin(380, cy, "r", port, "tl"))
        b.append(pl([(380, cy), (450, cy)], "cusb"))
        b.append(t(415, cy - 8, f"porta {port}", "ts", "middle"))
        b.append(block(450, cy - 35, 240, 70))
        b += [t(464, cy - 12, name, "tb"), t(464, cy + 6, dev, "ts"), t(464, cy + 22, prog, "ts")]
        n = len(wires)
        for i, (cls, _label) in enumerate(wires):
            y = cy + (i - (n - 1) / 2) * 10
            b.append(pl([(690, y), (890, y)], cls))
        b.append(t(790, cy - (n - 1) * 5 - 8, " · ".join(lbl for _c, lbl in wires), "tm", "middle"))
    b.append(block(890, 50, 170, 240))
    b += [t(904, 76, "PROTOBOARD", "tb"), t(904, 96, "LINHA do CAN", "ts"), t(904, 112, "3 diodos BAT43", "ts"),
          t(904, 128, "R1 de 1 kΩ", "ts"), t(904, 152, "GPIO27:", "ts"), t(904, 168, "10 kΩ e LED", "ts"),
          t(904, 192, "folhas 2, 3 e 4", "ts")]
    return svg(1080, 320, b, "Notebook ligado a um hub USB com três placas ESP32, todas ligadas à mesma protoboard.", 800)


# ---------- sheet 2: the bench bus ----------

def fig_bus():
    b = []
    LY = 200
    b.append(pl([(120, LY), (1030, LY)], "cline"))
    b.append(t(136, LY - 12, "LINHA", "tl fline"))
    b.append(t(960, LY - 12, "parada ≈ 3,3 V · com um TX em 0 ≈ 0,45 V", "ts", "end"))
    # R1, fed by the 3V3 of the firmware board
    b.append(pl([(70, 400), (40, 400), (40, 80), (120, 80), (120, 117)], "c33"))
    b.append(t(48, 70, "3V3 só da placa do firmware", "ts f33"))
    b.append(res(120, 135, v=True))
    b.append(t(134, 132, "R1", "tl"))
    b.append(t(134, 148, "1 kΩ", "ts"))
    b.append(pl([(120, 153), (120, LY)], "cline"))
    b.append(dot(120, LY, "cline"))
    boards = [
        (70, "PLACA DO FIRMWARE", "/dev/vguard-esp32", "GPIO26", "GPIO25", "D1", False),
        (380, "ADAPTADOR USB-CAN", "invertido: TX 26 · RX 25", "GPIO25", "GPIO26", "D2", True),
        (690, "GERADOR DE TRÁFEGO", "/dev/vguard-gerador", "GPIO26", "GPIO25", "D3", False),
    ]
    for x0, name, sub, rx, tx, dn, swapped in boards:
        rxx, txx = x0 + 90, x0 + 190
        b.append(pl([(rxx, 380), (rxx, LY)], "crx"))
        b.append(dot(rxx, LY, "cline"))
        b.append(pl([(txx, 380), (txx, 290)], "ctx"))
        b.append(pl([(txx, 270), (txx, LY)], "cline"))
        b.append(dot(txx, LY, "cline"))
        b.append(diode_v(txx, 280))
        b.append(t(txx + 15, 278, f"{dn} · BAT43", "t"))
        b.append(t(txx + 15, 294, "faixa do lado do TX", "ts"))
        b.append(block(x0, 380, 260, 100))
        pc = "t fwarn" if swapped else "t"
        b.append(pin(rxx, 380, "t", f"{rx} · RX", pc))
        b.append(pin(txx, 380, "t", f"{tx} · TX", pc))
        b.append(t(x0 + 14, 428, name, "tb"))
        b.append(t(x0 + 14, 446, sub, "ts fwarn" if swapped else "ts"))
        b.append(pl([(x0 + 130, 480), (x0 + 130, 510)], "cgnd"))
        b.append(pin(x0 + 130, 480, "b", "GND"))
    b.append(pin(70, 400, "l", "3V3", "t f33"))
    b.append(pl([(200, 510), (1050, 510)], "cgnd"))
    b.append(t(210, 530, "GND comum: um fio do GND de cada placa", "ts"))
    b.append(pl([(1050, 510), (1050, 238)], "cgnd", dash=True))
    b.append('<circle class="openend" cx="1050" cy="231" r="6"/>')
    b.append(t(1064, 292, "ponta solta", "ts"))
    b.append(t(1064, 308, "do teste", "ts"))
    b.append(t(1064, 324, "“linha presa”", "ts"))
    return svg(1160, 545, b, "Três placas na mesma LINHA: cada RX direto na linha, cada TX por um diodo BAT43, e um resistor de 1 kΩ que segura a linha no 3,3 V da placa do firmware.", 900)


# ---------- sheet 3: GPIO27 and the release LED ----------

def fig_gpio27():
    b = []
    b.append(pl([(100, 50), (430, 50)], "c33"))
    b.append(t(100, 38, "3V3 da placa do firmware", "ts f33"))
    b.append(dot(180, 50, "c33"))
    b.append(pl([(180, 50), (180, 97)], "c33"))
    b.append(res(180, 115, v=True))
    b.append(t(196, 112, "10 kΩ", "t"))
    b.append(t(196, 128, "segura o 1", "ts"))
    b.append(pl([(180, 133), (180, 220), (350, 220)], "c27"))
    b.append(dot(350, 50, "c33"))
    b.append(pl([(350, 50), (350, 87)], "c33"))
    b.append(res(350, 105, v=True))
    b.append(t(366, 109, "1 kΩ", "t"))
    b.append(pl([(350, 123), (350, 150)], "cmisc"))
    b.append(led_v(350, 160))
    b.append(t(378, 158, "LED de liberação", "t"))
    b.append(t(378, 174, "ânodo (perna longa) em cima", "ts"))
    b.append(pl([(350, 170), (350, 220)], "c27"))
    b.append(dot(265, 220, "c27"))
    b.append(dot(350, 220, "c27"))
    b.append(pl([(265, 220), (265, 262)], "c27"))
    b.append(pl([(350, 220), (470, 220)], "c27", dash=True))
    b.append(t(480, 216, "na sessão com transceptor,", "ts"))
    b.append(t(480, 232, "um fio vai ao JP1 (folha 8)", "ts"))
    b.append(block(140, 262, 250, 62))
    b.append(pin(265, 262, "t", "GPIO27 · open-drain", "t f27"))
    b.append(t(154, 312, "PLACA DO FIRMWARE", "tb"))
    return svg(700, 340, b, "O GPIO27 com um resistor de 10 kΩ para o 3,3 V e um LED com 1 kΩ: o LED acende quando o firmware põe o pino em 0.", 560)


# ---------- sheet 4: the breadboard, hole by hole ----------

def X(c):
    return 80 + (c - 1) * 20


ROW = {"a": 100, "b": 120, "c": 140, "d": 160, "e": 180, "f": 220, "g": 240, "h": 260, "i": 280, "j": 300}
RAIL = {"gt": 40, "33": 60, "line": 340, "gb": 360}


def diode_phys(c, label):
    x = X(c)
    return (f'<line class="lead" x1="{x}" y1="{ROW["j"]}" x2="{x}" y2="{RAIL["line"]}"/>'
            f'<rect class="dbody" x="{x-5}" y="309" width="10" height="22" rx="3"/>'
            f'<rect class="dstripe" x="{x-5}" y="310" width="10" height="4"/>'
            + t(x + 9, 326, label, "tm"))


def res_phys_v(x, y1, y2, label, lx, anchor):
    m = (y1 + y2) / 2
    return (f'<line class="lead" x1="{x}" y1="{y1}" x2="{x}" y2="{y2}"/>'
            f'<rect class="rbody" x="{x-5}" y="{m-10:g}" width="10" height="20" rx="4"/>'
            + t(lx, 84, label, "tm", anchor))


def res_phys_h(x1, x2, y, label):
    m = (x1 + x2) / 2
    return (f'<line class="lead" x1="{x1}" y1="{y}" x2="{x2}" y2="{y}"/>'
            f'<rect class="rbody" x="{m-17:g}" y="{y-6}" width="34" height="12" rx="5"/>'
            + t(m, y + 3.5, label, "rt", "middle"))


def fig_breadboard():
    b = []
    b.append('<rect class="bb" x="30" y="14" width="672" height="372" rx="10"/>')
    b.append('<rect class="channel" x="58" y="192" width="626" height="16" rx="3"/>')
    b.append('<line class="wire cgnd" x1="80" y1="28" x2="660" y2="28"/>')
    b.append('<line class="wire c33" x1="80" y1="72" x2="660" y2="72"/>')
    b.append('<line class="wire cline" x1="80" y1="328" x2="660" y2="328"/>')
    b.append('<line class="wire cgnd" x1="80" y1="372" x2="660" y2="372"/>')
    for key, label, cls in (("gt", "GND", "tm"), ("33", "3V3", "tm f33"), ("line", "LINHA", "tm fline"),
                            ("gb", "GND", "tm")):
        b.append(t(672, RAIL[key] + 3.5, label, cls))
    holes = []
    for c in range(1, 31):
        for y in list(ROW.values()) + list(RAIL.values()):
            holes.append(f'<rect class="hole" x="{X(c)-3}" y="{y-3}" width="6" height="6" rx="1"/>')
    b.append("".join(holes))
    for r, y in ROW.items():
        b.append(t(64, y + 3.5, r, "tm", "middle"))
    for c in (1, 5, 10, 15, 20, 25, 30):
        b.append(t(X(c), 88, str(c), "tm", "middle"))
        b.append(t(X(c), 318, str(c), "tm", "middle"))
    # the phase-4 area
    b.append(f'<rect class="region" x="{X(2)-12}" y="90" width="{X(18)-X(2)+24}" height="86" rx="6"/>')
    b.append(t(X(2) - 6, 171, "só na fase 4 · folha 6", "tm fwarn"))
    # jumpers with both ends on the breadboard
    b.append(path(f"M{X(1)},{RAIL['gt']} C 20,{RAIL['gt']+60} 20,{RAIL['gb']-60} {X(1)},{RAIL['gb']}", "cgnd"))
    b.append(path(f"M{X(24)},{ROW['e']} C {X(24)+26},{ROW['e']+50} {X(25)+26},{RAIL['line']-60} {X(25)},{RAIL['line']}",
                  "cline"))
    b.append(path(f"M{X(23)},{RAIL['gb']} C {X(23)+4},{RAIL['gb']-10} {X(23)+10},{RAIL['line']-8} {X(23)+12},{RAIL['line']-22}",
                  "cgnd", dash=True))
    b.append(f'<circle class="openend" cx="{X(23)+13}" cy="{RAIL["line"]-27}" r="4"/>')
    b.append(t(X(23) + 8, 400, "ponta solta “linha presa”", "tm", "middle"))
    # parts
    for c, label in ((3, "D1"), (10, "D2"), (17, "D3")):
        b.append(diode_phys(c, label))
    b.append(res_phys_v(X(24), RAIL["33"], ROW["a"], "R1 1k", X(24) - 6, "end"))
    b.append(res_phys_v(X(28), RAIL["33"], ROW["a"], "10k", X(28) - 6, "end"))
    b.append(res_phys_v(X(30), RAIL["33"], ROW["a"], "1k", X(30) + 7, "start"))
    for c1, c2 in ((2, 6), (8, 12), (14, 18)):
        b.append(res_phys_h(X(c1), X(c2), ROW["c"], "1k"))
    # the LED: anode in column 30, cathode in 28
    b.append(f'<line class="lead" x1="{X(30)}" y1="{ROW["d"]}" x2="{X(29)+6}" y2="148"/>')
    b.append(f'<line class="lead" x1="{X(28)}" y1="{ROW["d"]}" x2="{X(29)-6}" y2="148"/>')
    b.append(f'<circle class="ledbody" cx="{X(29)}" cy="146" r="8"/>')
    b.append(t(X(30) + 8, ROW["d"] + 4, "+", "tm"))
    # wires from the boards and the Pi: the number is the row of the table under the sheet
    ends = [
        (X(22), RAIL["33"], 1, "c33"), (X(6), RAIL["gb"], 2, "cgnd"), (X(3), ROW["f"], 3, "ctx"),
        (X(5), RAIL["line"], 4, "crx"), (X(28), ROW["e"], 5, "c27"), (X(13), RAIL["gb"], 6, "cgnd"),
        (X(10), ROW["f"], 7, "ctx"), (X(12), RAIL["line"], 8, "crx"), (X(20), RAIL["gb"], 9, "cgnd"),
        (X(17), ROW["f"], 10, "ctx"), (X(19), RAIL["line"], 11, "crx"),
        (X(2), ROW["a"], 12, "ctx"), (X(6), ROW["a"], 13, "ctx"), (X(8), ROW["a"], 14, "crx"),
        (X(12), ROW["a"], 15, "crx"), (X(14), ROW["a"], 16, "cpulse"), (X(18), ROW["a"], 17, "cpulse"),
        (X(20), RAIL["gt"], 18, "cgnd"),
    ]
    for x, y, n, cls in ends:
        b.append(num(x, y, n, cls))
    return svg(730, 410, b, "Uma protoboard de 400 furos com os diodos, os resistores, o LED e os pontos onde entra cada fio numerado.", 680)


# ---------- sheet 5: the Pi header ----------

def pi_header(cx, title, hi, notes):
    b = []
    xo, xe = cx - 10, cx + 10

    def y(p):
        return 64 + ((p + 1) // 2 - 1) * 14

    b.append(t(cx, 22, title, "tb", "middle"))
    b.append(f'<rect class="header" x="{cx-22}" y="52" width="44" height="290" rx="4"/>')
    for p in range(1, 41):
        x = xo if p % 2 else xe
        if p == 1:
            b.append(f'<rect class="hpin" x="{x-4.5}" y="{y(p)-4.5:g}" width="9" height="9"/>')
        else:
            b.append(f'<circle class="hpin" cx="{x}" cy="{y(p):g}" r="4.5"/>')
    b.append(t(xo - 14, y(1) + 4, "1", "tm", "end"))
    b.append(t(xe + 14, y(2) + 4, "2", "tm"))
    for p, cls, label in hi:
        x = xo if p % 2 else xe
        b.append(f'<circle class="hpinhi {cls}" cx="{x}" cy="{y(p):g}" r="6"/>')
        if p % 2:
            b.append(t(xo - 14, y(p) + 4, label, f"t f{cls[1:]}", "end"))
        else:
            b.append(t(xe + 14, y(p) + 4, label, f"t f{cls[1:]}"))
    for i, line in enumerate(notes):
        b.append(t(cx, 362 + i * 16, line, "ts", "middle"))
    return b, y


def fig_pi():
    b = []
    # No ESP32 in the loop test, so the pins take no TX or RX color.
    left, y = pi_header(200, "FASE 3 · TESTE DO LAÇO",
                        [(33, "cmisc", "33 · RX da UART5"), (32, "cmisc", "32 · TX da UART5")],
                        ["um jumper do pino 32 ao 33;", "tire antes de ligar o ESP32"])
    b += left
    b.append(path(f"M190,{y(33):g} C 168,{y(33)+18:g} 236,{y(32)-14:g} 210,{y(32):g}", "cjump"))
    right, _ = pi_header(560, "FASE 4 · FIOS DO ESP32",
                         [(2, "c5v", "2 · 5 V (só sem o USB)"), (4, "c5v", "4 · 5 V (ou este)"),
                          (32, "crx", "32 → GPIO16 do ESP32"), (33, "ctx", "GPIO17 do ESP32 → 33"),
                          (34, "cgnd", "34 · GND"), (36, "cpulse", "36 ← GPIO18 (pulso)")],
                         ["Pi vista de cima;", "o pino 1 tem a solda quadrada"])
    b += right
    return svg(780, 400, b, "O conector de 40 pinos da Pi duas vezes: na fase 3, um jumper liga os pinos 32 e 33; na fase 4, os pinos 2, 4, 32, 33, 34 e 36 recebem os fios do ESP32.", 640)


# ---------- sheet 6: the wires to the Pi ----------

def fig_phase4():
    b = []
    b.append(block(40, 50, 250, 350))
    b += [t(54, 76, "PLACA DO FIRMWARE", "tb"), t(54, 94, "UART2 até a Pi", "ts")]
    b.append(pl([(165, 50), (165, 18)], "cusb"))
    b.append(t(175, 26, "USB → notebook: IMU_SIM (ADR-0009)", "ts"))
    b.append(t(54, 376, "GPIO25 e GPIO26 seguem", "ts"))
    b.append(t(54, 392, "na LINHA da bancada (folha 2)", "ts"))
    b.append(block(700, 50, 260, 350))
    b += [t(714, 76, "RASPBERRY PI 4", "tb"), t(714, 94, "/dev/ttyAMA5 · UART5", "ts")]
    lines = [
        (130, "ctx", "GPIO17 · TX2", "pino 33 · GPIO13 · RX", "dados do ESP32 para a Pi", 1, True),
        (190, "crx", "GPIO16 · RX2", "pino 32 · GPIO12 · TX", "dados da Pi para o ESP32", -1, True),
        (250, "cpulse", "GPIO18 · pulso", "pino 36 · GPIO16", "só no build de teste", 1, True),
        (310, "cgnd", "GND", "pino 34 · GND", "GND comum", 0, False),
    ]
    for y, cls, esp, pi, what, d, has_res in lines:
        b.append(pin(290, y, "r", esp, f"t f{cls[1:]}"))
        b.append(pin(700, y, "l", pi, f"t f{cls[1:]}"))
        if has_res:
            b.append(pl([(290, y), (472, y)], cls))
            b.append(res(490, y))
            b.append(t(490, y - 14, "1 kΩ", "t", "middle"))
            b.append(pl([(508, y), (700, y)], cls))
        else:
            b.append(pl([(290, y), (700, y)], cls))
        b.append(t(360, y - 10, what, "ts", "middle"))
        if d:
            b.append(arrow(610, y, d, cls))
    b.append('<rect class="region" x="452" y="104" width="76" height="164" rx="6"/>')
    b.append(t(490, 286, "coluna da fase 4", "ts", "middle"))
    y5 = 360
    b.append(pin(290, y5, "r", "5V / VIN", "t f5v"))
    b.append(pin(700, y5, "l", "pino 2 ou 4 · 5 V", "t f5v"))
    b.append(pl([(290, y5), (542, y5)], "c5v", dash=True))
    b.append(ptc(560, y5))
    b.append(t(560, y5 - 14, "PTC 0,5 A", "t", "middle"))
    b.append(pl([(578, y5), (700, y5)], "c5v", dash=True))
    b.append(arrow(640, y5, -1, "c5v"))
    b.append(t(400, y5 - 10, "só sem o USB", "tl fwarn", "middle"))
    return svg(1000, 420, b, "A placa do firmware ligada à Pi: GPIO17 ao pino 33, GPIO16 ao pino 32 e GPIO18 ao pino 36, cada um por 1 kΩ; GND ao pino 34; e o 5 V da Pi, com um PTC de 0,5 A, só sem o USB.", 780)


# ---------- sheet 7: preparing the CAN module ----------

def fig_module():
    b = []
    b.append('<rect class="pcb" x="30" y="40" width="400" height="330" rx="12"/>')
    for cx, cy in ((48, 58), (412, 58), (48, 352), (412, 352)):
        b.append(f'<circle class="mhole" cx="{cx}" cy="{cy}" r="7"/>')
    b.append(t(46, 30, "MÓDULO MCP2515 + TJA1050 · desenho genérico", "ts"))
    labels = [("VCC", "c5v"), ("GND", "cgnd"), ("CS", None), ("SO", None), ("SI", None), ("SCK", None), ("INT", None)]
    for i, (lbl, cls) in enumerate(labels):
        y = 100 + i * 28
        b.append(f'<rect class="hpin" x="52" y="{y-5}" width="10" height="10"/>')
        b.append(t(72, y + 4, lbl, f"t f{cls[1:]}" if cls else "ts"))
    b.append(mark(60, 76, "G"))
    # the module's traces: MCP2515 pad 1 goes to TXD, pad 2 to RXD
    b.append(path("M162,89 L150,89 L150,248 L282,248", "trace"))
    b.append(path("M162,102 L156,102 L156,290 L282,290", "trace"))
    b.append('<rect class="ic" x="170" y="80" width="100" height="120" rx="3"/>')
    for i in range(9):
        y = 86 + i * 13
        b.append(f'<rect class="icpin" x="162" y="{y-3}" width="8" height="6"/>')
        b.append(f'<rect class="icpin" x="270" y="{y-3}" width="8" height="6"/>')
    b.append('<circle class="icdot" cx="180" cy="90" r="3"/>')
    b.append(t(220, 72, "MCP2515", "tl", "middle"))
    b.append('<line class="xmark" x1="176" y1="86" x2="264" y2="194"/><line class="xmark" x1="264" y1="86" x2="176" y2="194"/>')
    b.append(mark(296, 74, "A"))
    b.append('<rect class="padhi ctx" x="159" y="85" width="12" height="8"/>')
    b.append('<rect class="padhi crx" x="159" y="98" width="12" height="8"/>')
    b.append(mark(128, 82, "B"))
    b.append(mark(128, 110, "C"))
    b.append('<ellipse class="xtal" cx="222" cy="222" rx="20" ry="8"/>')
    b.append(t(222, 240, "8 MHz", "tm", "middle"))
    b.append('<rect class="ic" x="290" y="240" width="60" height="60" rx="3"/>')
    for i in range(4):
        y = 248 + i * 14
        b.append(f'<rect class="icpin" x="282" y="{y-3}" width="8" height="6"/>')
        b.append(f'<rect class="icpin" x="350" y="{y-3}" width="8" height="6"/>')
    b.append('<circle class="icdot" cx="298" cy="249" r="2.6"/>')
    b.append(t(320, 274, "TJA", "tic", "middle"))
    b.append(t(320, 287, "1050", "tic", "middle"))
    b.append('<rect class="padhi c27" x="349" y="244" width="10" height="8"/>')
    b.append(mark(380, 236, "D"))
    b.append(jumper(352, 124, "J1"))
    b.append(mark(316, 124, "E"))
    b.append('<rect class="borne" x="392" y="176" width="32" height="108" rx="3"/>')
    for cy, lbl, cls in ((204, "H", "fcanh"), (256, "L", "fcanl")):
        b.append(f'<circle class="screw" cx="408" cy="{cy}" r="9"/>')
        b.append(f'<line class="compline" x1="402" y1="{cy-6}" x2="414" y2="{cy+6}"/>')
        b.append(t(384, cy + 4, lbl, f"tl {cls}", "end"))
    b.append(mark(408, 158, "F"))
    # the TJA1050 up close
    b.append(t(700, 50, "TJA1050 · VISTO DE CIMA", "tb", "middle"))
    b.append('<rect class="ic" x="640" y="80" width="120" height="250" rx="6"/>')
    b.append('<circle class="icdot" cx="658" cy="100" r="5"/>')
    left = [(1, "TXD", "do TX do ESP32", "ctx"), (2, "GND", "GND", "cgnd"), (3, "VCC", "5 V", "c5v"),
            (4, "RXD", "sai em 5 V: divisor", "crx")]
    right = [(8, "S", "1 = silencioso", "c27"), (7, "CANH", "par trançado", "ccanh"), (6, "CANL", "par trançado", "ccanl"),
             (5, "Vref", "fica solta", None)]
    for i, ((pn, name, hint, cls), (pn2, name2, hint2, cls2)) in enumerate(zip(left, right)):
        y = 116 + i * 58
        b.append(f'<rect class="icpin big" x="618" y="{y-7}" width="22" height="14"/>')
        b.append(f'<rect class="icpin big" x="760" y="{y-7}" width="22" height="14"/>')
        b.append(t(629, y + 4, pn, "tpin", "middle"))
        b.append(t(771, y + 4, pn2, "tpin", "middle"))
        b.append(t(606, y, name, f"tl f{cls[1:]}", "end"))
        b.append(t(606, y + 16, hint, "ts", "end"))
        b.append(t(794, y, name2, f"tl f{cls2[1:]}" if cls2 else "tl", "start"))
        b.append(t(794, y + 16, hint2, "ts", "start"))
    return svg(940, 390, b, "O módulo CAN com o MCP2515 riscado, as ilhas 1 e 2 do MCP2515, a perna 8 do TJA1050, o jumper J1 e o borne; ao lado, os 8 pinos do TJA1050.", 760)


# ---------- sheet 8: the transceiver session ----------

def session_row(y0, firmware):
    b = []
    name = "PLACA DO FIRMWARE" if firmware else "ADAPTADOR USB-CAN"
    module = "MÓDULO A" if firmware else "MÓDULO B"
    tx, rx = ("GPIO25", "GPIO26") if firmware else ("GPIO26", "GPIO25")
    head = "LADO DO FIRMWARE" if firmware else "LADO DO ADAPTADOR · o TX e o RX trocam, e o S vai ao GND pelo JP2"
    b.append(t(20, y0 + 26, head, "tb"))
    b.append(block(20, y0 + 50, 190, 460))
    b.append(t(34, y0 + 74, name, "tb"))
    b.append(block(470, y0 + 50, 170, 460))
    b.append(t(484, y0 + 74, module, "tb"))
    b.append(t(484, y0 + 92, "TJA1050, sem o MCP2515", "ts"))
    # 3V3
    b.append(pin(210, y0 + 110, "r", "3V3", "t f33"))
    b.append(pl([(210, y0 + 110), (246, y0 + 110)], "c33"))
    b.append(tag(246, y0 + 110, "3V3", "c33"))
    # TX → TXD, with 10 kΩ to 3.3 V
    yt = y0 + 160
    b.append(pin(210, yt, "r", f"{tx} · TX", "t ftx"))
    b.append(pin(470, yt, "l", "TXD · ilha 1", "t ftx"))
    b.append(pl([(210, yt), (470, yt)], "ctx"))
    b.append(flag(340, y0 + 104, "3V3", "c33"))
    b.append(pl([(340, y0 + 104), (340, y0 + 116)], "c33"))
    b.append(res(340, y0 + 134, v=True))
    b.append(t(354, y0 + 138, "10 kΩ", "t"))
    b.append(pl([(340, y0 + 152), (340, yt)], "ctx"))
    b.append(dot(340, yt, "ctx"))
    # RXD → divider → RX
    yr = y0 + 240
    b.append(pin(470, yr, "l", "RXD · ilha 2", "t frx"))
    b.append(pin(210, yr, "r", f"{rx} · RX", "t frx"))
    b.append(pl([(470, yr), (428, yr)], "crx"))
    b.append(res(410, yr))
    b.append(t(410, yr - 13, "10 kΩ", "t", "middle"))
    b.append(t(452, yr + 22, "5 V", "ts fwarn", "middle"))
    b.append(pl([(392, yr), (210, yr)], "crx"))
    b.append(t(270, yr - 9, "≈ 3,3 V", "ts", "middle"))
    b.append(dot(340, yr, "crx"))
    b.append(pl([(340, yr), (340, yr + 27)], "crx"))
    b.append(res(340, yr + 45, v=True))
    b.append(t(354, yr + 49, "20 kΩ", "t"))
    b.append(pl([(340, yr + 63), (340, yr + 76)], "cgnd"))
    b.append(gnd(340, yr + 76))
    # S, with 10 kΩ to 3.3 V, and the jumper
    ys = y0 + 370
    b.append(pin(470, ys, "l", "S · perna 8", "t f27"))
    b.append(flag(420, ys - 58, "3V3", "c33"))
    b.append(pl([(420, ys - 58), (420, ys - 46)], "c33"))
    b.append(res(420, ys - 28, v=True))
    b.append(t(406, ys - 24, "10 kΩ", "t", "end"))
    b.append(pl([(420, ys - 10), (420, ys), (470, ys)], "c27"))
    b.append(dot(420, ys, "c27"))
    b.append(pl([(420, ys), (329, ys)], "c27"))
    if firmware:
        b.append(jumper(320, ys, "JP1"))
        b.append(pl([(311, ys), (210, ys)], "c27"))
        b.append(pin(210, ys, "r", "GPIO27", "t f27"))
        b.append(pl([(240, ys), (240, ys + 34)], "c27", dash=True))
        b.append(t(250, ys + 38, "também ao LED (folha 3)", "ts"))
    else:
        b.append(jumper(320, ys, "JP2"))
        b.append(pl([(311, ys), (268, ys), (268, ys + 8)], "cgnd"))
        b.append(gnd(268, ys + 8))
    # 5 V and GND
    yv, yg = y0 + 440, y0 + 490
    b.append(pin(210, yv, "r", "5V / VIN", "t f5v"))
    b.append(pin(470, yv, "l", "VCC", "t f5v"))
    b.append(pl([(210, yv), (470, yv)], "c5v"))
    b.append(t(340, yv - 9, "5 V · meça: no mínimo 4,75 V", "ts", "middle"))
    b.append(pin(210, yg, "r", "GND", "t"))
    b.append(pin(470, yg, "l", "GND", "t"))
    b.append(pl([(210, yg), (470, yg)], "cgnd"))
    # CANH, CANL and the 120 Ω J1
    yh, yl = y0 + 230, y0 + 290
    b.append(pl([(640, yh), (592, yh), (592, y0 + 242)], "ccanh"))
    b.append(res(592, y0 + 260, v=True))
    b.append(pl([(592, y0 + 278), (592, yl), (640, yl)], "ccanl"))
    b.append(t(580, y0 + 256, "J1", "tl", "end"))
    b.append(t(580, y0 + 272, "120 Ω", "ts", "end"))
    b.append(pin(640, yh, "r", "", "t"))
    b.append(pin(640, yl, "r", "", "t"))
    b.append(t(648, yh - 8, "CANH", "tl fcanh"))
    b.append(t(648, yl - 8, "CANL", "tl fcanl"))
    return b


def twist(xa, xb, y1, y2, n):
    """Two paths that cross n times between y1 and y2; with an even n, each ends where it began."""
    h = (y2 - y1) / n
    da, db = f"M{xa},{y1}", f"M{xb},{y1}"
    a, c = xa, xb
    for i in range(n):
        ya, yb = y1 + i * h, y1 + (i + 1) * h
        da += f" C{a},{ya + h/2:g} {c},{ya + h/2:g} {c},{yb:g}"
        db += f" C{c},{ya + h/2:g} {a},{ya + h/2:g} {a},{yb:g}"
        a, c = c, a
    return da, db


def fig_session():
    b = []
    b += session_row(0, True)
    b += session_row(560, False)
    b.append(pl([(640, 230), (700, 230), (700, 330)], "ccanh"))
    b.append(pl([(640, 290), (730, 290), (730, 330)], "ccanl"))
    dh, dl = twist(700, 730, 330, 770, 10)
    b.append(path(dh, "ccanh"))
    b.append(path(dl, "ccanl"))
    b.append(pl([(700, 770), (700, 790), (640, 790)], "ccanh"))
    b.append(pl([(730, 770), (730, 850), (640, 850)], "ccanl"))
    b.append(t(748, 520, "par", "ts"))
    b.append(t(748, 536, "trançado", "ts"))
    b.append(t(748, 552, "~50 cm", "ts"))
    return svg(830, 1080, b, "A placa do firmware com o módulo A e o adaptador com o módulo B, ligados por um par trançado de CANH e CANL. TXD com 10 kΩ para o 3,3 V, RXD por um divisor de 10 kΩ e 20 kΩ, e o pino S com 10 kΩ para o 3,3 V e um jumper: JP1 ao GPIO27 no firmware, JP2 ao GND no adaptador.", 640)


# ---------- sheet 9: the real IMU ----------

def fig_imu():
    b = []
    b.append(block(30, 30, 230, 250))
    b.append(t(44, 56, "PLACA DO FIRMWARE", "tb"))
    b.append(block(470, 30, 200, 250, dashed=True))
    b += [t(484, 56, "IMU REAL", "tb"), t(484, 74, "modelo a comprar (T14.1)", "ts"), t(484, 90, "lógica de 3,3 V", "ts")]
    rows = [(110, "c33", "3V3", "VCC", None, False), (150, "ci2c", "GPIO21 · SDA", "SDA", "I²C · fios até 20 cm", False),
            (190, "ci2c", "GPIO22 · SCL", "SCL", None, False),
            (230, "cmisc", "GPIO34 · INT", "INT, se tiver", "reservado, sem uso hoje", True),
            (262, "cgnd", "GND", "GND", None, False)]
    for y, cls, esp, imu, what, dash in rows:
        fc = "t" if cls in ("cgnd", "cmisc") else f"t f{cls[1:]}"
        b.append(pin(260, y, "r", esp, fc))
        b.append(pin(470, y, "l", imu, fc))
        b.append(pl([(260, y), (470, y)], cls, dash=dash))
        if what:
            b.append(t(365, y - 9, what, "ts", "middle"))
    return svg(700, 300, b, "Os pinos reservados para a IMU real: GPIO21 SDA, GPIO22 SCL, 3V3, GND e GPIO34 para o sinal de dado pronto.", 560)


FIGURES = {
    "desk": fig_desk, "bus": fig_bus, "gpio27": fig_gpio27, "breadboard": fig_breadboard, "pi": fig_pi,
    "phase4": fig_phase4, "module": fig_module, "session": fig_session, "imu": fig_imu,
}


def build():
    """The page, without the <html> and <head> that the claude.ai publisher adds."""
    page = open(os.path.join(HERE, "page.html"), encoding="utf-8").read()
    for name, figure in FIGURES.items():
        page = page.replace("{{" + name + "}}", figure())
    assert "{{" not in page, "a {{name}} in page.html has no figure"
    return page


def standalone(page):
    """The same page as a whole document, to open from the repository."""
    head, body = page.split('<div class="page">', 1)
    return ('<!doctype html>\n<html lang="pt-BR">\n<head>\n<meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
            f'{head}</head>\n<body>\n<div class="page">{body}</body>\n</html>\n')


def main():
    parser = argparse.ArgumentParser(description="Builds the wiring sheets of docs/BANCADA.md.")
    parser.add_argument("--fragment", metavar="PATH", help="also write the page without <html>/<head> here")
    args = parser.parse_args()
    page = build()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(standalone(page))
    print(OUT)
    if args.fragment:
        with open(args.fragment, "w", encoding="utf-8") as f:
            f.write(page)
        print(args.fragment)


if __name__ == "__main__":
    main()
