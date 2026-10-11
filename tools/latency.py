import re, sys
# For each 'SEND: <cmd>' in a ttserial log, report time until the first subsequent device line (response latency)
lines = open(sys.argv[1]).read().splitlines()
ts = lambda l: float(re.match(r'\[\s*([\d.]+)s\]', l).group(1)) if re.match(r'\[\s*([\d.]+)s\]', l) else None
rows = []
i = 0
while i < len(lines):
    m = re.match(r'>>> \[\s*([\d.]+)s\] SEND: (.*)', lines[i])
    if m:
        t0 = float(m.group(1)); cmd = m.group(2); j = i + 1; resp = None
        while j < len(lines):
            t = ts(lines[j])
            if t is not None and t >= t0 and lines[j].split('] ',1)[1].strip():
                resp = (t - t0) * 1000; break
            j += 1
        rows.append((t0, cmd, resp))
    i += 1
for t0, cmd, resp in rows:
    print(f"t={t0:7.2f}s  cmd={cmd:<45} first-response latency = {('%.0f ms' % resp) if resp is not None else 'NONE'}")
