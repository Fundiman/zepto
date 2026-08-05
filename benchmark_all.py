import os, sys, time, tempfile, sqlite3
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, r"E:\ProjectsWrongDimension\zepto")
import zepto as z

N = 500_000
CSV = r"E:\ProjectsWrongDimension\zepto\benchmark.csv"

OUT = tempfile.mkdtemp(prefix="bench_all_")

def fmt_col(vals, kind):
    return vals.tolist()

# ---------------- datasets (same seeds as original benches) ----------------
rng = np.random.default_rng(42)
cats = np.array([f"cat_{i}" for i in range(20)])
cities = np.array([f"city_{i:03d}" for i in range(200)])
skus = np.array([f"SKU-{i:07d}" for i in range(N)])

d6 = {
    "category": cats[rng.integers(0, 20, N)],
    "city": cities[rng.integers(0, 200, N)],
    "sku": skus[rng.permutation(N)],
    "qty": rng.integers(1, 101, N, dtype=np.int32),
    "price": np.round(rng.uniform(1, 500, N), 2),
    "ts": (1700000000 + rng.integers(0, 10**7, N)).astype(np.int64),
}

rng7 = np.random.default_rng(7)
dA = {
    "qty": rng7.integers(1, 101, N, dtype=np.int32),
    "price": np.round(rng7.uniform(1, 500, N), 2),
    "ts": (1700000000 + rng7.integers(0, 10**7, N)).astype(np.int64),
}
dB = {
    "cat": cats[rng7.integers(0, 20, N)],
    "city": cities[rng7.integers(0, 200, N)],
    "sku": skus[rng7.permutation(N)],
}

DATASETS = {
    "mixed6": {
        "df": d6,
        "zcols": [
            ("category", z.ColumnType.STRING, False, z.Encoding.DICT),
            ("city", z.ColumnType.STRING, False, z.Encoding.DICT),
            ("sku", z.ColumnType.STRING, False, z.Encoding.PLAIN),
            ("qty", z.ColumnType.I32, False, z.Encoding.BIT_PACKED),
            ("price", z.ColumnType.F64, False, z.Encoding.PLAIN),
            ("ts", z.ColumnType.I64, False, z.Encoding.PLAIN),
        ],
        "sql": "category TEXT, city TEXT, sku TEXT, qty INTEGER, price REAL, ts INTEGER",
        "pq_dict": True,
    },
    "numeric": {
        "df": dA,
        "zcols": [
            ("qty", z.ColumnType.I32, False, z.Encoding.BIT_PACKED),
            ("price", z.ColumnType.F64, False, z.Encoding.PLAIN),
            ("ts", z.ColumnType.I64, False, z.Encoding.PLAIN),
        ],
        "sql": "qty INTEGER, price REAL, ts INTEGER",
        "pq_dict": True,
    },
    "strings": {
        "df": dB,
        "zcols": [
            ("cat", z.ColumnType.STRING, False, z.Encoding.DICT),
            ("city", z.ColumnType.STRING, False, z.Encoding.DICT),
            ("sku", z.ColumnType.STRING, False, z.Encoding.PLAIN),
        ],
        "sql": "cat TEXT, city TEXT, sku TEXT",
        "pq_dict": True,
    },
    "sku": {
        "df": {"sku": skus},
        "zcols": [("sku", z.ColumnType.STRING, False, z.Encoding.PLAIN)],
        "sql": "sku TEXT",
        "pq_dict": False,
    },
}

# ---------------- engines ----------------
def bench_zepto(name, dset):
    cols = dset["zcols"]
    df = dset["df"]
    rows = list(zip(*[df[nm].tolist() for nm, _, _, _ in cols]))
    p = os.path.join(OUT, f"{name}-zepto.zepto")
    t0 = time.perf_counter()
    z.write(p, cols, rows, codec="zstd", use_rs=False)
    dt = time.perf_counter() - t0
    return os.path.getsize(p), dt

def bench_parquet(name, dset, codec):
    df = dset["df"]
    p = os.path.join(OUT, f"{name}-pq-{codec.lower()}.parquet")
    table = pa.Table.from_pandas(pd.DataFrame(df), preserve_index=False)
    t0 = time.perf_counter()
    pq.write_table(table, p, compression=codec, use_dictionary=dset["pq_dict"], version="2.6")
    dt = time.perf_counter() - t0
    return os.path.getsize(p), dt

def bench_sqlite(name, dset):
    cols = [c.split()[0] for c in dset["sql"].split(",")]
    df = dset["df"]
    p = os.path.join(OUT, f"{name}-sqlite.db")
    if os.path.exists(p):
        os.remove(p)
    con = sqlite3.connect(p)
    t0 = time.perf_counter()
    con.execute(f"CREATE TABLE t ({dset['sql']})")
    con.executemany(
        f"INSERT INTO t VALUES ({','.join('?' * len(cols))})",
        zip(*[df[c].tolist() for c in cols]),
    )
    con.commit()
    dt = time.perf_counter() - t0
    con.close()
    return os.path.getsize(p), dt

# ---------------- run ----------------
rows_out = []
for name, dset in DATASETS.items():
    print(f"\n=== {name} ===")
    for codec in ["zstd"]:
        sz, dt = bench_zepto(name, dset)
        rows_out.append((name, "zepto", codec, sz, dt))
        print(f"  zepto  {codec:6s} {sz:9d} B {dt*1000:7.1f} ms")
    for codec in ["NONE", "SNAPPY", "ZSTD", "GZIP", "BROTLI"]:
        sz, dt = bench_parquet(name, dset, codec)
        rows_out.append((name, "parquet", codec.lower(), sz, dt))
        print(f"  parquet {codec:6s} {sz:9d} B {dt*1000:7.1f} ms")
    sz, dt = bench_sqlite(name, dset)
    rows_out.append((name, "sqlite3", "default", sz, dt))
    print(f"  sqlite3 {'default':4s} {sz:9d} B {dt*1000:7.1f} ms")

# ---------------- write csv ----------------
with open(CSV, "w", newline="") as f:
    f.write("dataset,rows,engine,codec,size_bytes,write_ms\n")
    for name, engine, codec, sz, dt in rows_out:
        f.write(f"{name},{N},{engine},{codec},{sz},{dt*1000:.1f}\n")

print(f"\nwrote {CSV}")
print(open(CSV).read())
