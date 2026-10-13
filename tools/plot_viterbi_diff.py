import csv
import os
import sys
from pathlib import Path

out_png = Path('artifacts/punct_6/viterbi_diff_0_40.png')
csv_file = Path('artifacts/punct_6/viterbi_bit_metrics.csv')

if not csv_file.exists():
    print(f"CSV not found: {csv_file}")
    sys.exit(2)

# read CSV
rows = []
with csv_file.open('r', newline='') as f:
    reader = csv.DictReader(f)
    for r in reader:
        try:
            frame = int(r['frame'])
            pos = int(r['pos'])
            s0 = int(r['s0'])
            s1 = int(r['s1'])
        except Exception as e:
            continue
        rows.append((frame,pos,s0,s1))

# select frame 0
sel = [r for r in rows if r[0]==0 and 0 <= r[1] <= 40]
if not sel:
    print('No data for frame=0 pos 0..40')
    sys.exit(3)

# convert unsigned 16-bit to signed
def to_signed(u):
    if u >= 32768:
        return u - 65536
    return u

pos_list = [r[1] for r in sel]
s0s = [to_signed(r[2]) for r in sel]
s1s = [to_signed(r[3]) for r in sel]
diffs = [a-b for a,b in zip(s0s,s1s)]

# plotting
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
except Exception as e:
    print('matplotlib not available:', e)
    print('Run: python -m pip install matplotlib')
    sys.exit(4)

plt.figure(figsize=(9,4))
plt.plot(pos_list, diffs, marker='o', linestyle='-')
plt.axhline(0, color='gray', linewidth=0.8)
plt.xlabel('pos')
plt.ylabel('s0 - s1 (signed)')
plt.title('Viterbi metric differences (frame=0) pos 0..40')
# annotate FULLIDX
for x in (12,18,24):
    if x in pos_list:
        y = diffs[pos_list.index(x)]
        plt.annotate(str(x), xy=(x,y), xytext=(x,y + (100 if y>=0 else -100)),
                     arrowprops=dict(arrowstyle='->', lw=0.6), fontsize=8)

plt.grid(True, linestyle=':')
plt.tight_layout()

out_png.parent.mkdir(parents=True, exist_ok=True)
plt.savefig(out_png, dpi=150)
print('WROTE', out_png)
