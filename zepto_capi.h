#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define ZEPTO_API __declspec(dllexport)
#else
#define ZEPTO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- Version ----

#define ZEPTO_VERSION_MAJOR 1
#define ZEPTO_VERSION_MINOR 0
#define ZEPTO_VERSION_PATCH 0

ZEPTO_API int zepto_version(void);
ZEPTO_API const char* zepto_version_string(void);

// ---- Error codes ----

enum {
    ZEPTO_OK = 0,
    ZEPTO_ERROR = -1,
    ZEPTO_NOT_FOUND = -2,
    ZEPTO_CORRUPT = -3,
    ZEPTO_IO_ERROR = -4,
    ZEPTO_BAD_SCHEMA = -5,
    ZEPTO_BAD_ARGS = -6,
};

ZEPTO_API const char* zepto_error_message(int error_code);

// ---- Row-based API (legacy) ----

ZEPTO_API int zepto_write(
    const char* path,
    const char** col_names,
    const int* col_types,
    const int* col_nullable,
    const int* col_encodings,
    int num_cols,
    const uint8_t* row_data,
    size_t row_data_size,
    int num_rows
);

typedef struct {
    uint8_t* row_data;
    size_t   row_data_size;
    int      num_cols;
    int      num_rows;
    char**   col_names;
    int*     col_types;
} zepto_read_result;

ZEPTO_API int zepto_read(const char* path, zepto_read_result* out);
ZEPTO_API void zepto_read_free(zepto_read_result* res);

// ---- Database (opaque handle) ----

typedef struct zepto_db zepto_db;

ZEPTO_API zepto_db* zepto_db_open(const char* dir);
ZEPTO_API void      zepto_db_close(zepto_db* db);
ZEPTO_API int       zepto_db_exec(zepto_db* db, const char* sql);
ZEPTO_API int       zepto_db_snapshot(zepto_db* db, const char* name);
ZEPTO_API int       zepto_db_restore(zepto_db* db, const char* name);
ZEPTO_API char**    zepto_db_list_snapshots(zepto_db* db, int* out_count);
ZEPTO_API void      zepto_free_strings(char** strs, int count);

// ---- Query results (from last SELECT exec) ----

ZEPTO_API int zepto_db_query_col_count(zepto_db* db);
ZEPTO_API int zepto_db_query_row_count(zepto_db* db);
ZEPTO_API const char* zepto_db_query_col_name(zepto_db* db, int col_index);
ZEPTO_API int zepto_db_query_col_type(zepto_db* db, int col_index);
ZEPTO_API const char* zepto_db_query_value(zepto_db* db, int row_index, int col_index);

// ---- Columnar file API ----

ZEPTO_API int zepto_write_cols(
    const char* path,
    const char** col_names,
    const int* col_types,
    const int* col_nullable,
    const int* col_encodings,
    int num_cols,
    const uint8_t** col_data,
    const size_t* col_data_sizes,
    const uint8_t** null_bitmaps,
    int num_rows,
    int use_rs,
    int codec // 0=none, 1=lz4, 2=zstd
);

typedef struct {
    uint8_t** col_data;
    size_t*   col_data_sizes;
    uint8_t** null_bitmaps;
    int       num_cols;
    int       num_rows;
    char**    col_names;
    int*      col_types;
} zepto_read_cols_result;

ZEPTO_API int zepto_read_cols(const char* path, zepto_read_cols_result* out);
ZEPTO_API void zepto_read_cols_free(zepto_read_cols_result* res);

// ---- Filtered query API (zone-map pushdown) ----

ZEPTO_API int zepto_query_cols(
    const char* path,
    int num_predicates,
    const int* pred_col,
    const int* pred_op,       // 0=EQ,1=NE,2=GT,3=GE,4=LT,5=LE
    const int* pred_val_type, // 0=int32, 1=int64, 2=float32, 3=float64, 4=string
    const int64_t* pred_val_i64,
    const double* pred_val_f64,
    const char** pred_val_str,
    zepto_read_cols_result* out
);

// ---- Schema introspection ----

ZEPTO_API int zepto_file_col_count(const char* path);
ZEPTO_API int zepto_file_row_count(const char* path);
ZEPTO_API int zepto_file_col_type(const char* path, int col_index);
ZEPTO_API const char* zepto_file_col_name(const char* path, int col_index);

#ifdef __cplusplus
}
#endif
