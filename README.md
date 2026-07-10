# zepto — columnar-optimized storage with Reed-Solomon ECC

Single-header-plus-source C++20 library for columnar storage with CRC32C integrity, Reed-Solomon error correction (QR-code style GF(256)), zone-map pruning, and an optional WAL+CoW database layer with a SQL-like REPL.

## Build

```sh
make          # both zepto.o and zepto.exe
make lib      # zepto.o only (library)
make repl     # zepto.exe only (REPL)
make python   # zepto.dll (Python bindings via ctypes)
make clean
```

Requires GCC with C++20 and SSE 4.2 support. No external dependencies.

---

## Library API

### Types

| Type | Description |
|------|-------------|
| `ColumnType` | `I32`, `I64`, `F32`, `F64`, `STRING` |
| `Encoding` | `PLAIN`, `DICT`, `BIT_PACKED`, `DELTA` |
| `Value` | `std::variant<monostate, int32_t, int64_t, float, double, string>` — nullable by using `monostate` |
| `Row` | `struct { vector<Value> columns }` |
| `ColMeta` | `{ type, nullable, name, encoding }` |
| `PageHeader` | Page offset, sizes, CRC32C, type, encoding, value count, validity bitmap size |
| `ZoneMap` | Per-page min/max for integer, float, and string columns |
| `ChunkMeta` | Chunk offset, size, pages, zone maps, ECC parity |

### Writer — write columnar files

```cpp
zepto::Writer w("output.zepto");

// Define schema
w.add_column("name",  zepto::ColumnType::STRING, true, zepto::Encoding::DICT);
w.add_column("age",   zepto::ColumnType::I32,    false, zepto::Encoding::BIT_PACKED);
w.add_column("salary", zepto::ColumnType::F64);

// Append rows
w.append_row({std::monostate{}, int32_t(30), 75000.5});      // null name
w.append_row({std::string("bob"), int32_t(25), 82000.3});

w.close(); // flushes pending chunks + metadata
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
- **CoW** — checkpoints flush to `data.zepto` via `Writer`; snapshot management preserves point-in-time copies
- **SQL-like** — built-in parser for common operations

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
CREATE TABLE name (col TYPE [NOT NULL] [DICT|BIT_PACKED], ...)
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
ALTER TABLE name ADD COLUMN col TYPE [NOT NULL] [DICT|BIT_PACKED]
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
.checkpoint          — flush memory to data.zepto
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
1. Loads the latest `data.zepto` checkpoint (if any)
2. Replays WAL entries after the last checkpoint sequence number

Uncheckpointed mutations survive crashes (the WAL is `fsync`-flushed on every append).

### CoW Snapshots

- `.snapshot` copies `data.zepto` to `snapshots/<name>.zepto`
- `.restore` copies a snapshot back, truncates the WAL, and recovers
- `ROLLBACK` discards in-memory changes since the last checkpoint by re-reading `data.zepto`

---

## File Format

```
[Header: 24 bytes]
  magic(4) = "ZEPT"
  version(2)
  num_columns(2)
  total_rows(8)
  num_chunks(4)
  meta_size(4)

[Metadata]
  per-column: type(1) + flags(1) + name_len(2) + name + encoding(1)
  crc32c(4)

[Chunks...]
  per chunk:
    magic(4) + chunk_index(2) + num_cols(2) + num_rows(4) + dir_offset(4)
    [page data ...]
    [page directory ...]
    chunk_crc32c(4)
    ecc_size(2)
    [RS ECC parity]
```

- Pages within a chunk are column slices — one page per column per chunk
- Valid null bitmaps precede column data in nullable columns
- Zone maps (min/max) are stored per page in the directory
- Reed-Solomon parity (32 bytes per 223-byte block) enables chunk-level error recovery

---

---

## Python Bindings

Build `zepto.dll` via `make python`, then use the `zepto` package from the repo root:

```python
import zepto

# Write columnar file
cols = [
    ('name', zepto.ColumnType.STRING, True, zepto.Encoding.DICT),
    ('age',  zepto.ColumnType.I32,    False, zepto.Encoding.PLAIN),
]
rows = [['alice', 30], ['bob', 25], [None, 35]]
zepto.write('data.zepto', cols, rows)

# Read all rows
cols, rows = zepto.read('data.zepto')
for row in rows:
    print(row)  # ['alice', 30], ['bob', 25], [None, 35]

# WAL+CoW Database
db = zepto.Database('mydb')
db.exec('CREATE TABLE users (name STRING, age I32, salary F64)')
db.exec("INSERT INTO users VALUES ('alice', 30, 80000)")
db.exec("INSERT INTO users VALUES ('bob', 25, 55000)")
db.exec("INSERT INTO users VALUES ('charlie', 30, 90000)")
db.exec("SELECT name, UPPER(name), AVG(salary) FROM users GROUP BY name")
db.snapshot('v1')
db.exec('DELETE FROM users WHERE age > 25')
db.restore('v1')
print(db.list_snapshots())  # ['v1']
db.close()
```

All SQL features (aggregates, GROUP BY, HAVING, ORDER BY, scalar functions, aliases, ALTER TABLE, etc.) work through `db.exec()`. No Python dependencies — uses only `ctypes` from the standard library.

## Implementation Notes

- **CRC32C**: SSE 4.2 hardware acceleration with software fallback table
- **Bit-packing**: For integer columns with `BIT_PACKED` encoding; adaptive bit width based on value range
- **Dictionary encoding**: For `STRING` columns with `DICT` encoding; deduplicates within each chunk
- **Reed-Solomon**: GF(256) with primitive polynomial 0x11D; Berlekamp-Massey + Chien search + Forney algorithm for decoding
- **WAL**: Binary format — CRC(4) + seq(8) + op(1) + plen(4) + payload; flushed after each write
- **Single translation unit**: All code in `zepto.h` + `zepto.cpp`; REPL `main()` guarded by `-DZEPTO_REPL`
- **C API + Python bindings**: `zepto_capi.h` / `zepto_capi.cpp` exposes a C ABI; `zepto/__init__.py` wraps it via `ctypes` (zero Python dependencies)
