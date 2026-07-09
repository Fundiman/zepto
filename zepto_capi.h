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

ZEPTO_API void* zepto_db_open(const char* dir);
ZEPTO_API void  zepto_db_close(void* db);
ZEPTO_API int   zepto_db_exec(void* db, const char* sql);
ZEPTO_API int   zepto_db_query(void* db, const char* sql, zepto_read_result* out);
ZEPTO_API int   zepto_db_snapshot(void* db, const char* name);
ZEPTO_API int   zepto_db_restore(void* db, const char* name);
ZEPTO_API char** zepto_db_list_snapshots(void* db, int* out_count);
ZEPTO_API void  zepto_free_strings(char** strs, int count);

#ifdef __cplusplus
}
#endif
