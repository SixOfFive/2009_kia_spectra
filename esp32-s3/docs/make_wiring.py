import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle

BG = "#fbf7ec"; HOLE = "#c9c2ad"; INK = "#222"

fig, ax = plt.subplots(figsize=(12.5, 9.2))
ax.set_xlim(-1.2, 19.6); ax.set_ylim(-7.4, 9.6); ax.axis("off")

# ---------- titles ----------
ax.text(9.2, 9.1, "ESP32-S3  Voltage-Divider  Breadboard Layout", ha="center",
        fontsize=17, fontweight="bold", color=INK)
ax.text(9.2, 8.4, "Same circuit for bench & car — only the V+ source and the board's power change",
        ha="center", fontsize=10.5, color="#555")

# ---------- breadboard body (top half, rows a-e, cols 1-18) ----------
cols = list(range(1, 19))
rowy = {"a": 5.0, "b": 4.0, "c": 3.0, "d": 2.0, "e": 1.0}
ax.add_patch(FancyBboxPatch((0.3, 0.4), 18.4, 5.25, boxstyle="round,pad=0.1,rounding_size=0.2",
             facecolor=BG, edgecolor="#d8cfb6", lw=1.5, zorder=0))

# highlighted node columns
for cx, c in {6: "#ffd9d9", 10: "#fff4c2", 14: "#d6e8ff"}.items():
    ax.add_patch(Rectangle((cx - 0.42, 0.6), 0.84, 4.85, facecolor=c, edgecolor="none", zorder=1))

# holes (scatter keeps them round regardless of aspect)
hx = [cx for cx in cols for _ in rowy]
hy = [ry for _ in cols for ry in rowy.values()]
ax.scatter(hx, hy, s=46, c=HOLE, zorder=2, edgecolors="none")

# numbers + row letters
for cx in cols:
    ax.text(cx, 5.78, str(cx), ha="center", fontsize=7, color="#aaa")
for r, ry in rowy.items():
    ax.text(-0.1, ry, r, ha="center", va="center", fontsize=8, color="#aaa")

ax.text(6, 6.2, "V+  in", ha="center", fontsize=10, fontweight="bold", color="#b23")
ax.text(10, 6.2, "node M  (sense)", ha="center", fontsize=10, fontweight="bold", color="#a80")
ax.text(14, 6.2, "GND", ha="center", fontsize=10, fontweight="bold", color="#268")

# ---------- component helpers ----------
def lead(x1, y1, x2, y2):
    ax.plot([x1, x2], [y1, y2], color="#999", lw=2, zorder=3, solid_capstyle="round")

def resistor(xL, xR, y, bands, label, value):
    lead(xL, y, xL + 0.55, y); lead(xR - 0.55, y, xR, y)
    bx, bw = xL + 0.55, (xR - 0.55) - (xL + 0.55)
    ax.add_patch(FancyBboxPatch((bx, y - 0.28), bw, 0.56, boxstyle="round,pad=0.02,rounding_size=0.1",
                 facecolor="#e9d8a6", edgecolor="#b59b56", lw=1.2, zorder=4))
    n = len(bands)
    for i, c in enumerate(bands):
        ax.add_patch(Rectangle((bx + (i + 1) * bw / (n + 1) - 0.05, y - 0.26), 0.11, 0.52,
                     facecolor=c, edgecolor="none", zorder=5))
    ax.text((xL + xR) / 2, y + 0.5, label, ha="center", fontsize=10.5, fontweight="bold")
    ax.text((xL + xR) / 2, y - 0.6, value, ha="center", fontsize=9.5, color="#555")

def cap(xL, xR, y, label, value):
    lead(xL, y, xL + 0.75, y); lead(xR - 0.75, y, xR, y)
    ax.add_patch(FancyBboxPatch((xL + 0.75, y - 0.32), (xR - 0.75) - (xL + 0.75), 0.64,
                 boxstyle="round,pad=0.02,rounding_size=0.16",
                 facecolor="#5b8fd6", edgecolor="#2f5a96", lw=1.2, zorder=4))
    ax.text((xL + xR) / 2, y, label, ha="center", va="center", fontsize=9.5,
            color="white", fontweight="bold", zorder=6)
    ax.text((xL + xR) / 2, y - 0.6, value, ha="center", fontsize=9.5, color="#555")

R1B = ["#6b3f1d", "#111111", "#2e7d32", "#d4af37"]   # brown black green gold = 1 MΩ
R2B = ["#d32f2f", "#d32f2f", "#f9a825", "#d4af37"]   # red red yellow gold  = 220 kΩ

resistor(6, 10, 5.0, R1B, "R1", "1 MΩ")              # row a : col 6 -> col 10
resistor(10, 14, 4.0, R2B, "R2", "220 kΩ")            # row b : col 10 -> col 14
cap(10, 14, 3.0, "C1", "100 nF")                      # row c : col 10 -> col 14

ax.text(2.4, 3.0, "each column's\nrows a-e are\nlinked inside", ha="center",
        fontsize=7.5, color="#b8b09a", style="italic")

# ---------- ESP32 block ----------
ax.add_patch(FancyBboxPatch((3.5, -3.3), 11.0, 2.4, boxstyle="round,pad=0.1,rounding_size=0.25",
             facecolor="#20242b", edgecolor="#3fb950", lw=2, zorder=3))
ax.text(9.0, -1.7, "ESP32-S3-N16R8  dev board", ha="center", fontsize=12.5,
        color="#e6edf3", fontweight="bold")
ax.text(9.0, -2.45, "find pins by their silkscreen labels — physical positions vary by board",
        ha="center", fontsize=8.2, color="#8b949e", style="italic")

def pin(x, name):
    ax.add_patch(Rectangle((x - 0.5, -0.9), 1.0, 0.34, facecolor="#cdd3da",
                 edgecolor="#888", zorder=4))
    ax.text(x, -0.73, name, ha="center", va="center", fontsize=9, fontweight="bold",
            color="#111", zorder=5)
pin(6, "5V"); pin(10, "IO1"); pin(14, "GND")

def wire(x, color, lbl):
    ax.plot([x, x], [-0.56, 1.0], color=color, lw=3.2, zorder=2, solid_capstyle="round")
    ax.scatter([x], [1.0], s=95, c=color, zorder=6, edgecolors="white", linewidths=0.8)
    ax.text(x + 0.28, 0.15, lbl, ha="left", va="center", fontsize=8, color=color, rotation=90)
wire(6, "#e5484d", "5V wire")
wire(10, "#caa100", "sense -> IO1")
wire(14, "#222222", "GND wire")

# ---------- notes ----------
note = (
"WIRING  (R1, R2, C1 are non-polarized - orientation does not matter):\n"
"   R1  1 MO    :  col 6 (V+)   ->  col 10 (node M)      [row a]\n"
"   R2  220 kO  :  col 10 (M)   ->  col 14 (GND)         [row b]\n"
"   C1  100 nF  :  col 10 (M)   ->  col 14 (GND)         [row c]\n"
"   wires       :  5V -> col 6      GPIO1(IO1) -> col 10      GND -> col 14\n"
"\n"
"BENCH (now) :  V+ = the board's 5V pin,  power the board over USB   ->  reads ~5.0 V\n"
"CAR (drop-in):  V+ = fused Battery+ (12-14.4 V),  power board from buck 5V,\n"
"                and tie Battery(-)/chassis to the board GND.\n"
"Never wire V+ straight to IO1 - only through the divider.  GPIO1 max 3.3 V."
)
ax.text(-1.0, -3.8, note, ha="left", va="top", fontsize=8.6, color="#333", family="monospace")

plt.savefig(r"C:\Users\sixoffive\Documents\Claude_Projects\esp32\docs\wiring-breadboard.png",
            dpi=145, bbox_inches="tight", facecolor="white")
print("saved")
