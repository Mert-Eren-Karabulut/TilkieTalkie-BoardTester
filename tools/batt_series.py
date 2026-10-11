import re, sys
# parse a ttserial log containing repeated 'battery' dumps; print a table of key values per dump
txt = open(sys.argv[1]).read()
dumps = txt.split('--- Battery Information ---')[1:]
def g(d, k, cast=float):
    m = re.search(k + r':\s*([-\d.]+)', d)
    return cast(m.group(1)) if m else None
print(f"{'#':>2} {'V_cell':>6} {'I_mA':>6} {'BAT_mV':>6} {'PACK_mV':>7} {'drop_mV':>7} {'R_mOhm':>6} {'Rem_mAh':>7} {'FCC':>5} {'SOC%':>5} {'T_C':>5} {'ICHG_reg':>8} state")
for i, d in enumerate(dumps):
    v = g(d, r'Voltage'); I = g(d, r'Battery Current', int); rem = g(d, r'Remaining Capacity', int); fcc = g(d, r'Full Charge Capacity', int)
    soc = g(d, r'Percentage'); t = g(d, r'Temperature')
    m = re.search(r'Gauge BAT/PACK:\s*(\d+)mV / (\d+)mV', d); bat, pack = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    m2 = re.search(r'REG00/01/03/06: 0x[0-9a-f]+ / 0x[0-9a-f]+ / 0x([0-9a-f]+)', d); ichg = int(m2.group(1), 16) * 10 if m2 else None
    st = re.search(r'Charge State:\s*([^\n]+)', d); st = st.group(1).strip() if st else '?'
    drop = (pack - bat) if (bat and pack) else None
    r = (drop / I * 1000) if (drop is not None and I) else None
    print(f"{i:>2} {v:>6.3f} {I:>6} {bat:>6} {pack:>7} {drop:>7} {r:>6.0f} {rem:>7} {fcc:>5} {soc:>5.1f} {t:>5.1f} {ichg:>8} {st}")
