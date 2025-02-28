/**
 * @file arrow.h
 * @author Ashot Vardanian
 * @date 08 Jun 2022
 * @addtogroup C
 *
 * @brief Implements bindings for the Apache Arrow.
 *
 * Internally replicates the bare-minimum defintions required
 * for Arrow to be ABI-compatible.
 *
 * https://arrow.apache.org/docs/format/CDataInterface.html#structure-definitions
 * https://arrow.apache.org/docs/format/CDataInterface.html#example-use-case
 *
 * After the data is exported into Arrow RecordBatches or Tables,
 * we can stream it with standardized messages:
 * https://arrow.apache.org/docs/format/Columnar.html#encapsulated-message-format
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h> // `int64_t`
#include <stdlib.h>   // `malloc`

#include "ustore/docs.h"

#if !__has_include("arrow/c/abi.h") && !defined(ARROW_C_DATA_INTERFACE)
#define ARROW_C_DATA_INTERFACE
#define ARROW_C_STREAM_INTERFACE
#endif

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

typedef struct ArrowSchema {
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;
    void (*release)(struct ArrowSchema*);
    void* private_data;
} ArrowSchema;

typedef struct ArrowArray {
    // Array data description
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    void const** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;
    void (*release)(struct ArrowArray*);
    void* private_data;
} ArrowArray;

#endif // ARROW_C_DATA_INTERFACE

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE

typedef struct ArrowArrayStream {
    int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);
    int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);
    char const* (*get_last_error)(struct ArrowArrayStream*);
    void (*release)(struct ArrowArrayStream*);
    void* private_data;
} ArrowArrayStream;

#endif // ARROW_C_STREAM_INTERFACE

/**
 * @brief Converts @c ustore_doc_field_type_t to a "format" string supported by Apache Arrow.
 */
static char const* ustore_doc_field_type_to_arrow_format(ustore_doc_field_type_t const field_type) {
    // Export the right format string and number of buffers to be managed by Arrow.
    // For scalar arrays we need: bitmap and data.
    // For variable length arrays we need: bitmap, @b offsets and data.
    // Important note, both 32-bit and 64-bit offsets are supported.
    // https://arrow.apache.org/docs/format/CDataInterface.html#data-type-description-format-strings
    // https://arrow.apache.org/docs/format/Columnar.html#buffer-listing-for-each-layout
    switch (field_type) {
    case ustore_doc_field_null_k: return "n";
    case ustore_doc_field_bool_k: return "b";
    // TODO: UUID logical type may be natively supported in Arrow vocabulary:
    // https://arrow.apache.org/docs/format/Columnar.html#extension-types
    case ustore_doc_field_uuid_k: return "w:16";
    case ustore_doc_field_i8_k: return "c";
    case ustore_doc_field_i16_k: return "s";
    case ustore_doc_field_i32_k: return "i";
    case ustore_doc_field_i64_k: return "l";
    case ustore_doc_field_u8_k: return "C";
    case ustore_doc_field_u16_k: return "S";
    case ustore_doc_field_u32_k: return "I";
    case ustore_doc_field_u64_k: return "L";
    case ustore_doc_field_f16_k: return "e";
    case ustore_doc_field_f32_k: return "f";
    case ustore_doc_field_f64_k: return "g";
    case ustore_doc_field_bin_k: return "z";
    case ustore_doc_field_str_k: return "u";
    default: return "";
    }
}

static void release_malloced_schema(struct ArrowSchema* schema) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
        struct ArrowSchema* child = schema->children[i];
        if (child && child->release != NULL)
            child->release(child);
        free(child);
    }
    free(schema->children);
    schema->release = NULL;
}

static void release_malloced_array(struct ArrowArray* array) {
    // Free children
    for (int64_t i = 0; i < array->n_children; ++i) {
        struct ArrowArray* child = array->children[i];
        if (child && child->release != NULL)
            child->release(child);
        free(child);
    }
    free(array->children);
    // Freeing buffers can be avoided, UStore still owns those regions
    // while the connection is alive and hasn't be reused for any other
    // requests.
    // for (int64_t i = 0; i < array->n_buffers; ++i)
    //     free((void*)array->buffers[i]);
    free(array->buffers);
    array->release = NULL;
}

/**
 * @brief Defines the structure of the continuous `arrow::RecordBatch`
 * represented in C as a combination of `ArrowSchema` and `ArrowArray`.
 */
static void ustore_to_arrow_schema( //
    ustore_size_t const docs_count,
    ustore_size_t const fields_count,

    struct ArrowSchema* schema,
    struct ArrowArray* array,
    ustore_error_t* error) {

    // Schema
    schema->format = "+s";
    schema->name = "";
    schema->metadata = NULL;
    schema->flags = 0;
    schema->n_children = (int64_t)(fields_count);
    schema->dictionary = NULL;
    schema->release = &release_malloced_schema;
    schema->children = (ArrowSchema**)malloc(sizeof(struct ArrowSchema*) * schema->n_children);

    // Data
    array->length = (int64_t)(docs_count);
    array->offset = 0;
    array->null_count = 0;
    array->n_buffers = 1;
    array->n_children = (int64_t)(fields_count);
    array->dictionary = NULL;
    array->release = &release_malloced_array;
    array->buffers = (void const**)malloc(sizeof(void*) * array->n_buffers);
    array->buffers[0] = NULL; // no presences, so bitmap can be omitted
    array->children = (ArrowArray**)malloc(sizeof(struct ArrowArray*) * array->n_children);

    if (!schema->children || !array->buffers || !array->children) {
        *error = "Failed to allocate memory";
        return;
    }

    // Allocate sub-schemas and sub-arrays
    // TODO: Don't malloc every child schema/array separately,
    // use `private_data` member for that.
    for (ustore_size_t field_idx = 0; field_idx != fields_count; ++field_idx)
        if (!(schema->children[field_idx] = (ArrowSchema*)malloc(sizeof(struct ArrowSchema)))) {
            *error = "Failed to allocate memory";
            return;
        }
    for (ustore_size_t field_idx = 0; field_idx != fields_count; ++field_idx)
        if (!(array->children[field_idx] = (ArrowArray*)malloc(sizeof(struct ArrowArray)))) {
            *error = "Failed to allocate memory";
            return;
        }
}

/**
 * @brief Fill a column in a continuous `arrow::RecordBatch`, pre-structured
 * by the `ustore_to_arrow_schema()` call. Supports scalar and string entries.
 * For lists use `ustore_to_arrow_list()`.
 */
static void ustore_to_arrow_column( //
    ustore_size_t const docs_count,
    ustore_str_view_t const field_name,
    ustore_doc_field_type_t const field_type,

    ustore_octet_t const* column_validities,
    ustore_length_t const* column_offsets,
    void const* column_contents,

    struct ArrowSchema* schema,
    struct ArrowArray* array,

    ustore_error_t* error) {

    schema->name = field_name;
    schema->metadata = NULL;
    schema->flags = column_validities ? ARROW_FLAG_NULLABLE : 0;
    schema->dictionary = NULL;
    schema->children = NULL;
    schema->release = &release_malloced_schema;
    schema->format = ustore_doc_field_type_to_arrow_format(field_type);
    schema->n_children = 0;

    // Export the data
    switch (field_type) {
    case ustore_doc_field_bool_k:
    case ustore_doc_field_uuid_k:
    case ustore_doc_field_i8_k:
    case ustore_doc_field_i16_k:
    case ustore_doc_field_i32_k:
    case ustore_doc_field_i64_k:
    case ustore_doc_field_u8_k:
    case ustore_doc_field_u16_k:
    case ustore_doc_field_u32_k:
    case ustore_doc_field_u64_k:
    case ustore_doc_field_f16_k:
    case ustore_doc_field_f32_k:
    case ustore_doc_field_f64_k: array->n_buffers = 2; break;
    case ustore_doc_field_bin_k:
    case ustore_doc_field_str_k: array->n_buffers = 3; break;
    case ustore_doc_field_null_k: array->n_buffers = 0; break;
    default: array->n_buffers = 0; break;
    }
    array->offset = 0;
    array->length = docs_count;
    array->null_count = column_validities ? -1 : 0;
    array->n_children = 0;
    array->dictionary = NULL;
    array->children = NULL;
    array->release = &release_malloced_array;

    // Link our buffers
    array->buffers = (void const**)malloc(sizeof(void*) * array->n_buffers);
    if (!array->buffers) {
        *error = "Failed to allocate memory";
        return;
    }

    if (array->n_buffers == 2) {
        array->buffers[0] = (void*)column_validities;
        array->buffers[1] = (void*)column_contents;
    }
    else {
        array->buffers[0] = (void*)column_validities;
        array->buffers[1] = (void*)column_offsets;
        array->buffers[2] = (void*)column_contents;
    }
}

/**
 * @brief Fill a column in a continuous `arrow::RecordBatch`, pre-structured
 * by the `ustore_to_arrow_schema()` call. Supports lists of scalars.
 * For regular scalars or strings use `ustore_to_arrow_column()`.
 */
static void ustore_to_arrow_list( //
    ustore_size_t const docs_count,
    ustore_str_view_t const field_name,
    ustore_doc_field_type_t const field_type,

    ustore_octet_t const* column_validities,
    ustore_length_t const* column_offsets,
    void const* column_contents,

    struct ArrowSchema* schema,
    struct ArrowArray* array,

    ustore_error_t* error) {

    // Allocate a sub-array
    // https://arrow.apache.org/docs/format/Columnar.html#variable-size-list-layout
    ustore_to_arrow_schema(docs_count, 1, schema, array, error);
    if (*error)
        return;

    schema->name = field_name;
    schema->metadata = NULL;
    schema->flags = column_validities ? ARROW_FLAG_NULLABLE : 0;
    schema->dictionary = NULL;
    schema->format = "+l";

    array->null_count = column_validities ? -1 : 0;
    array->n_buffers = 2;

    // Link our buffers
    if (array->buffers)
        free(array->buffers);
    array->buffers = (void const**)malloc(sizeof(void*) * array->n_buffers);
    if (!array->buffers) {
        *error = "Failed to allocate memory";
        return;
    }

    array->buffers[0] = (void*)column_validities;
    array->buffers[1] = (void*)column_offsets;
    ustore_to_arrow_column(column_offsets[docs_count],
                           "chunks",
                           field_type,
                           NULL,
                           NULL,
                           column_contents,
                           schema->children[0],
                           array->children[0],
                           error);
}

/**
 * @brief Placeholder for future streaming exports.
 */
static void ustore_to_arrow_stream( //
    ustore_database_t const,
    ustore_transaction_t const,
    ustore_size_t const,

    ustore_size_t const,
    ustore_key_t const,
    ustore_key_t const,

    ustore_collection_t const*,
    ustore_size_t const,

    ustore_str_view_t const*,
    ustore_size_t const,

    ustore_doc_field_type_t const*,
    ustore_size_t const,

    struct ArrowArrayStream*,
    ustore_arena_t*) {
}

static bool check_presence(ustore_octet_t const* begin, size_t idx) {
    return *(begin + idx / CHAR_BIT) & ((ustore_octet_t)(1 << (idx % CHAR_BIT)));
}

#ifdef __cplusplus
} /* end extern "C" */
#endif
