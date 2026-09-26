import sys
from pathlib import Path
p = Path(sys.argv[1]) if len(sys.argv)>1 else Path('artifacts/punct_6')
in_bits = (p / 'regression_frame_0_in_bits.txt').read_text().strip()
decoded = (p / 'regression_frame_0_decoded.txt').read_text().strip()
recon = [int(x.strip()) for x in (p / 'regression_frame_0_recon_costs.txt').read_text().splitlines() if x.strip()]

if len(in_bits) != len(decoded):
    print('length mismatch: in_bits', len(in_bits), 'decoded', len(decoded))

mismatches = []
for i, (a, b) in enumerate(zip(in_bits, decoded)):
    if a != b:
        group_start = i * 4
        group = recon[group_start:group_start+4]
        has_neutral = any(x == 32767 for x in group)
        fullinfo = not has_neutral
        mismatches.append((i, a, b, group, fullinfo))

print('total_mismatches=', len(mismatches))
fullinfo_count = sum(1 for m in mismatches if m[4])
print('fullinfo_mismatches=', fullinfo_count)
for idx, a, b, group, fullinfo in mismatches:
    print(f'idx={idx} v={a} o={b} fullinfo={fullinfo} group={group}')
