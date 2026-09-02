# zepto — columnar-optimized storage with Reed-Solomon ECC

Single-header-plus-source C++20 library for columnar storage with CRC32C integrity, Reed-Solomon error correction (QR-code style GF(256)), LZ4/ZSTD block compression, adaptive per-page encodings (bit-packing, delta, RLE, dictionary), zone-map pruning, and an optional WAL+CoW database layer with a SQL-like REPL and CLI.

## Build

```sh
make          # everything: lib + repl + tests + tools
make lib      # zepto.o only (library)
make repl     # zepto.exe only (REPL)
make test     # build + run quick/corrupt/rs/lz4/delta tests
make tools    # zcli.exe only (standalone CLI)
make python   # zepto.dll (Python bindings via ctypes)
make clean
```

Requires GCC with C++20 and SSE 4.2 support. Compression uses zstd 1.5.6, vendored in `third_party/zstd` — a `libzstd.dll` is copied next to the built binaries on Windows, while Linux/macOS link the system `libzstd`.

---

## Library API

### Types

| Type | Description |
|------|-------------|
| `ColumnType` | `I32`, `I64`, `F32`, `F64`, `STRING` |
| `Encoding` | `PLAIN`, `DICT`, `BIT_PACKED`, `DELTA`, `RLE` — integer pages auto-pick the smallest of PLAIN/BIT_PACKED/DELTA/RLE |
| `Codec` | `NONE`, `LZ4`, `ZSTD` — per-chunk block compression |
| `Value` | `std::variant<monostate, int32_t, int64_t, float, double, string>` — nullable by using `monostate` |
| `Row` | `struct { vector<Value> columns }` |
| `ColMeta` | `{ type, nullable, name, encoding }` |
| `PageHeader` | Page offset, sizes, CRC32C, type, encoding, value count, validity bitmap size |
| `ZoneMap` | Per-page min/max for integer, float, and string columns |
| `ChunkMeta` | Chunk index, offset, size, row count, pages, zone maps |

### Writer — write columnar files

```cpp
// path, chunk size, Reed-Solomon ECC on, ZSTD compression
zepto::Writer w("output.zepto", 64 * 1024 * 1024, true, zepto::Codec::ZSTD);

// Define schema
w.add_column("name",  zepto::ColumnType::STRING, true, zepto::Encoding::DICT);
w.add_column("age",   zepto::ColumnType::I32,    false, zepto::Encoding::BIT_PACKED);
w.add_column("salary", zepto::ColumnType::F64);

// Append rows
w.append_row({std::monostate{}, int32_t(30), 75000.5});      // null name
w.append_row({std::string("bob"), int32_t(25), 82000.3});

w.close(); // flushes pending chunks + metadata
```

For bulk loads, column pages are encoded in parallel from typed arrays — no `Value` variant overhead:

```cpp
std::vector<zepto::Writer::WriteColArray> cols;
cols.push_back({zepto::ColumnType::I32, (const uint8_t*)ages, validity, n}); // age values
// ... per-column typed arrays
w.write_columns(cols, n); // encodes all pages in parallel, one chunk
```

### Reader — read columnar files

```cpp
zepto::Reader r("output.zepto");
if (!r.open()) return;

// Schema introspection
for (size_t i = 0; i < r.column_types().size(); i++)
    printf("%s: %d\n", r.column_names()[i].c_str(), (int)r.column_types()[i]);

// Predicate pushdown with zone-map pruning
zepto::Query q;
q.predicates = {{1, zepto::Query::GT, int64_t(25)}};  // age > 25
q.select_columns = {0, 1};                              // only name, age
q.limit = 10;
auto result = r.query(q);
for (auto& row : result.rows) { /* ... */ }

// Sequential read
auto chunk = r.read_chunk(0);

// Typed columnar read — raw arrays per column, no variant overhead
auto cols = r.read_chunk_cols(0, {1, 2}); // age, salary
for (size_t i = 0; i < cols[0].num_values; i++) {
    int32_t age = ((const int32_t*)cols[0].data.data())[i];
    bool valid = (cols[0].validity[i / 8] >> (i % 8)) & 1;
}
```

### Reader — integrity verification

```cpp
bool ok = r.verify_integrity(); // CRC32C + ECC correction of all chunks
```

### Utility

```cpp
zepto::is_null(val); // true if Value holds monostate
```

---

## Database Layer (WAL + CoW)

### Overview

The `Database` class wraps `Writer`/`Reader` with:

- **WAL** — every mutation is CRC-protected and appended to `<dir>/wal` before being applied in-memory
- **CoW** — checkpoints flush to `data.zdb` via `Writer`; snapshot management preserves point-in-time copies
- **SQL-like** — built-in parser for common operations

`open()` accepts either a database **directory** (default layout: `data.zdb` + `wal` + `snapshots/`) or a **`.zdb` checkpoint file** — no directory required. A `.zdb` path names the checkpoint file; its supporting files live in a directory named after the file:

```
dbname.zdb          # what you pass to open()
dbname/
  wal/wal           # WAL
  data/dbname.zdb   # the checkpoint file
  snapshots/
```

If the pointed-at `.zdb` already exists as a literal file (not under `<name>/data/`), it is opened directly.

### Programmatic use

```cpp
zepto::Database db;
db.open("mydb");

db.exec("CREATE TABLE users (name STRING, age I32)");
db.exec("INSERT INTO users VALUES ('alice', 30)");
db.exec("INSERT INTO users VALUES ('bob', 25)");
db.exec("SELECT * FROM users WHERE age >= 30");

db.create_snapshot("v1");
db.checkpoint();
db.close();
```

### SQL Commands

```
CREATE TABLE name (col TYPE [NOT NULL] [DICT|BIT_PACKED|DELTA|RLE], ...)
INSERT INTO name VALUES (val, ...)
SELECT [DISTINCT] *|cols|agg(col)|UPPER(col)|LOWER(col)|LENGTH(col) [AS alias], ...
  FROM name
  [WHERE cond [AND|OR ...]]
  [GROUP BY col, ...]
  [HAVING cond]
  [ORDER BY col [ASC|DESC], ...]
  [LIMIT n]
  [OFFSET n]
UPDATE name SET col=val, ... WHERE cond
DELETE FROM name WHERE cond
DROP TABLE name
ALTER TABLE name ADD COLUMN col TYPE [NOT NULL] [DICT|BIT_PACKED|DELTA|RLE]
ALTER TABLE name DROP COLUMN col
BEGIN / COMMIT / ROLLBACK
```

Types: `I32`/`INT`, `I64`/`BIGINT`, `F32`/`FLOAT`, `F64`/`DOUBLE`, `STRING`/`TEXT`/`VARCHAR`

WHERE operators: `=` `!=` `<>` `<` `>` `<=` `>=` `LIKE` `NOT LIKE` `IS NULL` `IS NOT NULL` `IN` `NOT IN` `BETWEEN`

Aggregate functions: `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `AVG(col)`, `MIN(col)`, `MAX(col)`

Scalar functions: `UPPER(col)`, `LOWER(col)`, `LENGTH(col)`

Column aliases: `col AS alias`, `func(col) AS alias`

### Dot-Commands

```
.checkpoint          — flush memory to data.zdb
.snapshot <name>     — CoW copy of current state
.snapshots           — list snapshots
.restore <name>      — restore from a snapshot
.tables              — list tables (database directory name)
.schema              — show table DDL
.help                — command reference
.exit / .quit        — exit REPL
```

### WAL Recovery

On `open()`, the database:
1. Loads the latest `data.zdb` checkpoint (if any)
2. Replays WAL entries after the last checkpoint sequence number

Uncheckpointed mutations survive crashes (the WAL stream is flushed on every append).

### CoW Snapshots

- `.snapshot` copies `data.zdb` to `snapshots/<name>.zdb`
- `.restore` copies a snapshot back, removes the WAL, and recovers
- `ROLLBACK` discards in-memory changes since the last checkpoint by re-reading `data.zdb`

---

## File Format

```
[Header: 28 bytes]
  magic(4) = "ZEPT"
  version(2)             // 4
  num_columns(2)
  total_rows(8)
  num_chunks(4)
  meta_size(4)
  flags(4)               // bit 0 = RS-ECC, bit 1 = LZ4, bit 2 = ZSTD

[Metadata]
  per-column: type(1) + flags(1) + name_len(2) + name + encoding(1)
  crc32c(4)

[Chunks...]
  per chunk (raw buffer):
    magic(4) + chunk_index(2) + num_cols(2) + num_rows(4) + dir_offset(4)
    [page data ...]
    [page directory ...]
    chunk_crc32c(4)
```

On-disk each chunk is prefixed by `magic(4) + raw_size(4)` and then wrapped in one of three ways:

- **RS-ECC**: `raw_size(4)` + interleaved data+parity (32 parity bytes per 223-byte block) — chunk-level error correction
- **LZ4/ZSTD (VERSION 4)**: `comp_size(4)` + segmented container — the raw buffer is split into ~1MB segments, each compressed as its own frame on a worker thread, then stored as `nseg(2) + [seg_raw(4) + seg_comp(4)]*nseg + frames...`
- **None**: `comp_size(4) = raw_size(4)` + raw buffer

- Pages within a chunk are column slices — one page per column per chunk
- Valid null bitmaps precede column data in nullable columns
- Integer pages auto-select the smallest of `PLAIN` / `BIT_PACKED` (adaptive bit width) / `DELTA` / `RLE` (value/run-length pairs); `DICT` string indices are bit-packed per dictionary
- Zone maps (min/max) are stored per page in the directory

---

## Python Bindings

Build `zepto.dll` via `make python`, then use the `zepto` package from the repo root:

```python
import zepto

# Write columnar file (default codec lz4; pass codec='zstd'/'none' and use_rs=True as needed)
cols = [
    ('name', zepto.ColumnType.STRING, True, zepto.Encoding.DICT),
    ('age',  zepto.ColumnType.I32,    False, zepto.Encoding.BIT_PACKED),
]
rows = [['alice', 30], ['bob', 25], [None, 35]]
zepto.write('data.zepto', cols, rows)

# Read all rows
cols, rows = zepto.read('data.zepto')
for row in rows:
    print(row)  # ['alice', 30], ['bob', 25], [None, 35]

# Filtered query with zone-map pushdown
meta, rows = zepto.query('data.zepto', [(1, zepto.Op.GT, 25)])

# Schema peek without reading data
zepto.peek('data.zepto')  # {'columns': [...], 'num_rows': 3, 'num_cols': 2}
```

The `Database` class provides a sqlite3-like API with `exec()` (aliased as `execute()`), `executemany()` with `?` placeholders, and cursors (`fetchall()`, `fetchone()`, `fetchmany()`, `description`):

```python
db = zepto.Database('mydb')
db.exec('CREATE TABLE users (name STRING, age I32, salary F64)')
db.exec("INSERT INTO users VALUES ('alice', 30, 80000)")
db.exec("INSERT INTO users VALUES ('bob', 25, 55000)")
db.exec("INSERT INTO users VALUES ('charlie', 30, 90000)")
for row in db.execute("SELECT name, UPPER(name), AVG(salary) FROM users GROUP BY name").fetchall():
    print(row)
db.snapshot('v1')
db.exec('DELETE FROM users WHERE age > 25')
db.restore('v1')
print(db.list_snapshots())  # ['v1']
db.close()
```

All SQL features (aggregates, GROUP BY, HAVING, ORDER BY, scalar functions, aliases, ALTER TABLE, etc.) work through `db.exec()`. No Python dependencies — uses only `ctypes` from the standard library.

### Streaming SELECTs

`db.stream(sql)` (aliased as `exec_stream()`) runs a SELECT on a background worker thread and returns a `StreamCursor` that yields each matching row **as the scan finds it**, before the scan completes. This is useful for large scans where you want the first results immediately:

```python
cur = db.stream('SELECT id, name FROM users WHERE age >= 30')
cur.fetchone()   # first match, arrives as soon as the scan finds it
for row in cur:  # remaining rows, blocking as needed
    print(row)
```

`StreamCursor` supports `fetchone()`, `fetchmany(n)`, `fetchall()`, iteration, `description`, and `rowcount` (the number of rows yielded so far — it grows while the scan is still running). Call `close()` to stop the background worker (it blocks until the scan finishes). Rows are delivered progressively for plain scans with `WHERE`/`LIMIT`/`OFFSET`; queries that must see every row first — aggregates, `GROUP BY`, `HAVING`, `DISTINCT`, `ORDER BY` — still deliver correct results but buffer internally until the scan completes.

The async layer (`zepto.async_`) streams without blocking the event loop:

```python
import zepto.async_ as az

adb = await az.connect('mydb')
cur = await adb.stream('SELECT id, name FROM users')
async for row in cur:
    print(row)
```

`AsyncStreamCursor` mirrors `StreamCursor` with `await fetchone()/fetchmany()/fetchall()` and `async for`; `await cur.close()` stops the worker.

## Implementation Notes

- **CRC32C**: SSE 4.2 hardware acceleration with software fallback table
- **Adaptive integer encoding**: Each integer page is encoded with the smallest of `PLAIN`, `BIT_PACKED` (adaptive bit width from the value span), `DELTA` (monotonic sequences), and `RLE` (value/run-length pairs); `DELTA`/`RLE` apply as automatic upgrades on `BIT_PACKED`/`PLAIN` columns
- **Dictionary encoding**: For `STRING` columns with `DICT` encoding; deduplicates within each chunk and bit-packs the indices (LSB-first, width byte)
- **Codecs**: Per-chunk `LZ4` (built-in) or `ZSTD` (level 1, vendored in `third_party/zstd`) block compression
- **Parallelism**: Column pages are encoded and decoded in parallel, and compressed chunks are split into ~1MB frames that compress/decompress concurrently across `hardware_concurrency()` threads
- **Reed-Solomon**: GF(256) with primitive polynomial 0x11D; Berlekamp-Massey + Chien search + Forney algorithm for decoding; QR-code-style block interleaving spreads burst errors across blocks
- **WAL**: Binary format — CRC(4) + seq(8) + op(1) + plen(4) + payload; flushed after each write
- **Single translation unit**: All code in `zepto.h` + `zepto.cpp`; REPL `main()` guarded by `-DZEPTO_REPL`
- **C API + Python bindings**: `zepto_capi.h` / `zepto_capi.cpp` exposes a C ABI; `zepto/__init__.py` wraps it via `ctypes` (zero Python dependencies)
- **Tests**: `make test` runs quick_test, corrupt_test (ECC recovery), rs_test, lz4_test, and delta_test (DELTA/RLE roundtrips)

---

## CLI (`zcli`)

`make tools` builds a sqlite3-style interactive shell. Usage:

```
zcli [options] [database_dir]

  <dir>            Open (or create) a database directory
  -c <sql>         Execute a SQL statement, then exit
  -f <file>        Execute SQL from a file, then exit
  -timer           Show query timing
  -csv | -json     Set output mode
  -noheader        Hide column headers
  -h | -help       Show help
```

Interactive meta-commands include `.open`, `.read`, `.tables`, `.schema`, `.output table|csv|json`, `.timer`, `.headers`, `.checkpoint`, `.snapshot`, `.snapshots`, `.restore`, `.version`, `.quit`. Without an argument the shell starts against an in-memory database.

---

## Benchmarks

500k-row comparisons of zepto vs. pyarrow parquet (all codecs) and sqlite3 are in [benchmark.md](benchmark.md).
