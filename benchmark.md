# zepto benchmark (ran at 2026-8-5 6:31 AM GMT+05)

500,000 rows per dataset. Engines: **zepto** (current, zstd level 1), **parquet** (pyarrow 21), **sqlite3** (default pragmas, single transaction).

## mixed6

| engine | codec | size | write |
|---|---|---:|---:|
| parquet      | zstd       | 5.90 MiB | 252 ms | :star:
| **zepto**    | zstd       | 5.97 MiB | 991 ms |
| parquet      | brotli     | 6.10 MiB | 1390 ms |
| parquet      | gzip       | 6.29 MiB | 1311 ms |
| parquet      | snappy     | 8.30 MiB | 158 ms |
| parquet      | none       | 13.93 MiB | 129 ms |
| sqlite3      | default    | 24.27 MiB | 833 ms |

## numeric

| engine | codec | size | write |
|---|---|---:|---:|
| parquet      | zstd       | 3.32 MiB | 88 ms | :star:
| parquet      | brotli     | 3.45 MiB | 683 ms |
| **zepto**    | zstd       | 3.52 MiB | 485 ms |
| parquet      | gzip       | 3.62 MiB | 702 ms |
| parquet      | snappy     | 4.37 MiB | 78 ms |
| parquet      | none       | 5.85 MiB | 69 ms |
| sqlite3      | default    | 11.00 MiB | 564 ms |

## strings

| engine | codec | size | write |
|---|---|---:|---:|
| **zepto**    | zstd       | 2.44 MiB | 1060 ms | :star:
| parquet      | zstd       | 2.57 MiB | 153 ms |
| parquet      | brotli     | 2.61 MiB | 1059 ms |
| parquet      | gzip       | 2.67 MiB | 607 ms |
| parquet      | snappy     | 3.94 MiB | 157 ms |
| parquet      | none       | 8.08 MiB | 142 ms |
| sqlite3      | default    | 16.59 MiB | 761 ms |

## sku

| engine | codec | size | write |
|---|---|---:|---:|
| **zepto**    | zstd       | 103.0 KiB | 447 ms | :star:
| parquet      | zstd       | 160.4 KiB | 41 ms |
| parquet      | brotli     | 348.1 KiB | 596 ms |
| parquet      | gzip       | 1.12 MiB | 161 ms |
| parquet      | snappy     | 2.35 MiB | 37 ms |
| parquet      | none       | 7.15 MiB | 28 ms |
| sqlite3      | default    | 9.10 MiB | 504 ms |

## summary (zepto vs competitors)

| dataset | zepto | best competitor | zepto wins? |
|---|---|---:|:---|
| mixed6 | 5.97 MiB | 5.90 MiB (parquet-zstd) | no |
| numeric | 3.52 MiB | 3.32 MiB (parquet-zstd) | no |
| strings | 2.44 MiB | 2.57 MiB (parquet-zstd) | yes |
| sku | 103.0 KiB | 160.4 KiB (parquet-zstd) | yes |
