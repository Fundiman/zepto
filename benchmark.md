# zepto benchmark

500,000 rows per dataset. Engines: **zepto** (current, zstd level 1), **parquet** (pyarrow 21), **sqlite3** (default pragmas, single transaction).

## mixed6

| engine | codec | size | write |
|---|---|---:|---:|
| parquet      | zstd       | 5.90 MiB | 232 ms | :star:
| **zepto**    | zstd       | 5.98 MiB | 1253 ms |
| parquet      | brotli     | 6.10 MiB | 1411 ms |
| parquet      | gzip       | 6.29 MiB | 1135 ms |
| parquet      | snappy     | 8.30 MiB | 184 ms |
| parquet      | none       | 13.93 MiB | 195 ms |
| sqlite3      | default    | 24.27 MiB | 863 ms |

## numeric

| engine | codec | size | write |
|---|---|---:|---:|
| parquet      | zstd       | 3.32 MiB | 90 ms | :star:
| parquet      | brotli     | 3.45 MiB | 683 ms |
| **zepto**    | zstd       | 3.54 MiB | 405 ms |
| parquet      | gzip       | 3.62 MiB | 648 ms |
| parquet      | snappy     | 4.37 MiB | 64 ms |
| parquet      | none       | 5.85 MiB | 66 ms |
| sqlite3      | default    | 11.00 MiB | 512 ms |

## strings

| engine | codec | size | write |
|---|---|---:|---:|
| **zepto**    | zstd       | 2.43 MiB | 900 ms | :star:
| parquet      | zstd       | 2.57 MiB | 147 ms |
| parquet      | brotli     | 2.61 MiB | 794 ms |
| parquet      | gzip       | 2.67 MiB | 568 ms |
| parquet      | snappy     | 3.94 MiB | 123 ms |
| parquet      | none       | 8.08 MiB | 116 ms |
| sqlite3      | default    | 16.59 MiB | 667 ms |

## sku

| engine | codec | size | write |
|---|---|---:|---:|
| **zepto**    | zstd       | 137.1 KiB | 380 ms | :star:
| parquet      | zstd       | 160.4 KiB | 43 ms |
| parquet      | brotli     | 348.1 KiB | 576 ms |
| parquet      | gzip       | 1.12 MiB | 156 ms |
| parquet      | snappy     | 2.35 MiB | 34 ms |
| parquet      | none       | 7.15 MiB | 30 ms |
| sqlite3      | default    | 9.10 MiB | 454 ms |

## summary (zepto vs competitors)

| dataset | zepto | best competitor | zepto wins? |
|---|---|---:|:---|
| mixed6 | 5.98 MiB | 5.90 MiB (parquet-zstd) | no |
| numeric | 3.54 MiB | 3.32 MiB (parquet-zstd) | no |
| strings | 2.43 MiB | 2.57 MiB (parquet-zstd) | yes |
| sku | 137.1 KiB | 160.4 KiB (parquet-zstd) | yes |
