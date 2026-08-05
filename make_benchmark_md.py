import csv, io

CSV = r"E:\ProjectsWrongDimension\zepto\benchmark.csv"
MD = r"E:\ProjectsWrongDimension\zepto\benchmark.md"

with open(CSV) as f:
    body = [l for l in f if not l.lstrip().startswith("#")]
    rows = list(csv.DictReader(body))

datasets = []
for name in [r["dataset"] for r in rows]:
    if name not in datasets:
        datasets.append(name)

def fmt_b(n):
    if n >= 1024 * 1024:
        return f"{n / (1024*1024):.2f} MiB"
    return f"{n / 1024:.1f} KiB"

def fmt_ms(n):
    return f"{n:.0f} ms"

def pad(s, w):
    return s.ljust(w)

lines = []
lines.append("# zepto benchmark")
lines.append("")
lines.append("500,000 rows per dataset. Engines: **zepto** (current, zstd level 1), "
             "**parquet** (pyarrow 21), **sqlite3** (default pragmas, single transaction).")
lines.append("")

for ds in datasets:
    d = [r for r in rows if r["dataset"] == ds]
    d.sort(key=lambda r: float(r["size_bytes"]))
    best = d[0]
    lines.append(f"## {ds}")
    lines.append("")
    lines.append("| engine | codec | size | write |")
    lines.append("|---|---|---:|---:|")
    for r in d:
        engine = r["engine"]
        if engine == "zepto":
            engine = "**" + engine + "**"
        mark = " :star:" if r == best else ""
        lines.append(f"| {pad(engine, 12)} | {pad(r['codec'], 10)} | {fmt_b(int(r['size_bytes']))} | {fmt_ms(float(r['write_ms']))} |{mark}")
    lines.append("")

lines.append("## summary (zepto vs competitors)")
lines.append("")
lines.append("| dataset | zepto | best competitor | zepto wins? |")
lines.append("|---|---|---:|:---|")
for ds in datasets:
    d = [r for r in rows if r["dataset"] == ds]
    zr = next(r for r in d if r["engine"] == "zepto")
    comp = min((r for r in d if r["engine"] != "zepto"), key=lambda r: int(r["size_bytes"]))
    win = "yes" if int(zr["size_bytes"]) <= int(comp["size_bytes"]) else "no"
    lines.append(f"| {ds} | {fmt_b(int(zr['size_bytes']))} | {fmt_b(int(comp['size_bytes']))} ({comp['engine']}-{comp['codec']}) | {win} |")
lines.append("")

# end-to-end markdown
with open(MD, "w") as f:
    f.write("\n".join(lines))
print("\n".join(lines))
print("\nwrote", MD)
