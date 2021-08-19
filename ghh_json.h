#ifndef GHH_JSON_H
#define GHH_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

// licensing info at end of file.

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

typedef enum json_type {
	JSON_OBJECT,
	JSON_ARRAY,
	JSON_STRING,
	JSON_NUMBER,
	JSON_TRUE,
	JSON_FALSE,
	JSON_NULL
} json_type_e;

typedef struct json_object {
	union json_obj_data {
		struct json_hmap *hmap;
		struct json_vec *vec;
		char *string;
		double number;
	} data;

	json_type_e type;
} json_object_t;

typedef struct json {
	json_object_t *root;

	// page allocator
	char **pages; // fat ptr
	size_t cur_page, page_cap; // tracks allocator pages
	size_t used; // tracks current page stack
} json_t;

void json_load(json_t *, char *text);
void json_load_file(json_t *, const char *filepath);
void json_unload(json_t *);

void json_print(json_object_t *);
void json_fprint(FILE *, json_object_t *);

// take an object, retrieve data and cast
json_object_t *json_get_object(json_object_t *, char *key);
json_object_t **json_get_array(json_object_t *, char *key, size_t *out_size);
char *json_get_string(json_object_t *, char *key);
double json_get_number(json_object_t *, char *key);
bool json_get_bool(json_object_t *, char *key);

// cast object to data type
json_object_t **json_to_array(json_object_t *, size_t *out_size);
char *json_to_string(json_object_t *);
double json_to_number(json_object_t *);
bool json_to_bool(json_object_t *);

#ifdef GHH_JSON_IMPL

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// errors + debugging ==========================================================

#ifdef JSON_DEBUG_INFO
#define JSON_DEBUG(...) printf(__VA_ARGS__)
#else
#define JSON_DEBUG(...)
#endif

#ifndef NDEBUG
#define JSON_ASSERT(cond, ...) if (!(cond)) JSON_ERROR(__VA_ARGS__)
#else
#define JSON_ASSERT(...)
#endif

#define JSON_ERROR(...)\
    do {\
        fprintf(stderr, "JSON ERROR: ");\
        fprintf(stderr, __VA_ARGS__);\
        exit(-1);\
    } while (0)

#define JSON_CTX_ERROR(ctx, ...)\
	do {\
        fprintf(stderr, "JSON ERROR: ");\
        fprintf(stderr, __VA_ARGS__);\
		json_contextual_error(ctx);\
		exit(-1);\
	} while (0)

#ifndef NDEBUG
// could do this with an X macro but I think it would reduce clarity
static const char *json_types[] = {
	"JSON_OBJECT",
	"JSON_ARRAY",
	"JSON_STRING",
	"JSON_NUMBER",
	"JSON_TRUE",
	"JSON_FALSE",
	"JSON_NULL"
};
#endif

typedef struct json_ctx {
	json_t *json;
	const char *text;
	size_t index;
} json_ctx_t;

static void json_contextual_error(json_ctx_t *ctx) {
	// get line number and line index
	size_t line = 1, line_index = 0;

	for (size_t i = 0; i < ctx->index; ++i) {
		if (ctx->text[i] == '\n') {
			++line;
			line_index = i + 1;
		}
	}

	// get line length
	size_t i = line_index;
	int line_length = 0;

	while (ctx->text[i] != '\n' && ctx->text[i] != '\0') {
		++line_length;
		++i;
	}

	// print formatted line data
	printf("%6zu | %.*s\n", line, line_length, ctx->text + line_index);
	printf("%6s | %*s\n", "", (int)(ctx->index - line_index) + 1, "^");
}

// memory ======================================================================

#ifndef JSON_MALLOC
#define JSON_MALLOC(size) malloc(size)
#endif
#ifndef JSON_FREE
#define JSON_FREE(size) free(size)
#endif

// size of fread() buffer
#ifndef JSON_FREAD_BUF_SIZE
#define JSON_FREAD_BUF_SIZE 4096
#endif

// size of each json_t allocator page, increasing this results pretty directly
// in less cache misses
#ifndef JSON_PAGE_SIZE
#define JSON_PAGE_SIZE 65536
#endif

#define JSON_INIT_PAGE_CAP 8

// fat functions use fat pointers to track memory allocation with JSON_MALLOC
// and JSON_FREE. this is useful for allocating things that aren't on a json_t
// allocator
static void *json_fat_alloc(size_t size) {
	size_t *ptr = (size_t *)JSON_MALLOC(sizeof(*ptr) + size);

	*ptr++ = size;

	return ptr;
}

static inline void json_fat_free(void *ptr) {
	JSON_FREE((size_t *)ptr - 1);
}

static inline size_t json_fat_size(void *ptr) {
	return *((size_t *)ptr - 1);
}

static void *json_fat_realloc(void *ptr, size_t size) {
	void *new_ptr = json_fat_alloc(size);

	if (ptr) {
		memcpy(new_ptr, ptr, json_fat_size(ptr));
		json_fat_free(ptr);
	}

	return new_ptr;
}

// allocates on a json_t page
static void *json_page_alloc(json_t *json, size_t size) {
	// allocate new page when needed
	if (json->used + size > JSON_PAGE_SIZE) {
		if (size >= JSON_PAGE_SIZE) {
			JSON_DEBUG("allocating custom page and new page.\n");

			// size too big for pages, give this pointer its own page
			json->pages[++json->cur_page] = (char *)JSON_MALLOC(size);

			void *ptr = json->pages[json->cur_page];

			json->pages[++json->cur_page] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);
			json->used = 0;

			return ptr;
		} else {
			JSON_DEBUG("allocating new page.\n");

			// allocate new page
			if (++json->cur_page == json->page_cap) {
				json->page_cap <<= 1;
				json->pages = (char **)json_fat_realloc(
					json->pages,
					json->page_cap * sizeof(*json->pages)
				);
			}

			json->pages[json->cur_page] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);
			json->used = 0;
		}
	}

	// return page space
	void *ptr = json->pages[json->cur_page] + json->used;

	json->used += size;

	return ptr;
}

// array (vector) ==============================================================

typedef struct json_vec {
    void **data; // fat ptr
    size_t size, cap, min_cap;
} json_vec_t;

static void json_vec_alloc_one(json_vec_t *vec) {
    if (vec->size + 1 > vec->cap) {
        vec->cap <<= 1;
        vec->data = (void **)json_fat_realloc(
			vec->data,
			vec->cap * sizeof(*vec->data)
		);
    }
}

static void json_vec_free_one(json_vec_t *vec) {
	JSON_ASSERT(vec->size, "popped from vec of size zero.\n");

    if (vec->cap > vec->min_cap && vec->size < vec->cap >> 2) {
        vec->cap >>= 1;
        vec->data = (void **)json_fat_realloc(
			vec->data,
			vec->cap * sizeof(*vec->data)
		);
    }
}

static void json_vec_make(json_vec_t *vec, size_t init_cap) {
    vec->size = 0;
    vec->min_cap = vec->cap = init_cap;

    vec->data = (void **)json_fat_alloc(
		vec->cap * sizeof(*vec->data)
	);
}

static void json_vec_push(json_vec_t *vec, void *item) {
    json_vec_alloc_one(vec);
    vec->data[vec->size++] = item;
}

static void *json_vec_pop(json_vec_t *vec) {
    json_object_t *item = (json_object_t *)vec->data[vec->size - 1];

    json_vec_free_one(vec);
    --vec->size;

    return item;
}

static inline void *json_vec_peek(json_vec_t *vec) {
	return vec->data[vec->size - 1];
}

// hashmap =====================================================================

#if INTPTR_MAX == INT64_MAX
// 64 bit
typedef uint64_t json_hash_t;
#define JSON_FNV_PRIME 0x00000100000001b3
#define JSON_FNV_BASIS 0xcbf29ce484222325
#else
// 32 bit
typedef uint32_t json_hash_t;
#define JSON_FNV_PRIME 0x01000193a
#define JSON_FNV_BASIS 0x0811c9dc5
#endif

typedef struct json_hnode {
    json_object_t *object;
    json_hash_t hash;
    size_t index, steps;
} json_hnode_t;

typedef struct json_hmap {
	json_vec_t vec; // stores keys in order
    json_hnode_t *nodes; // fat ptr
    size_t size, cap, min_cap;
} json_hmap_t;

// fnv-1a hash function (http://isthe.com/chongo/tech/comp/fnv/)
static json_hash_t json_hash_str(char *str) {
    json_hash_t hash = JSON_FNV_BASIS;

    while (*str)
        hash = (hash ^ *str++) * JSON_FNV_PRIME;

    return hash;
}

static json_hnode_t *json_hnodes_alloc(size_t num_nodes) {
	json_hnode_t *nodes = (json_hnode_t *)json_fat_alloc(
		num_nodes * sizeof(json_hnode_t)
	);

	for (size_t i = 0; i < num_nodes; ++i)
		nodes[i].hash = 0;

	return nodes;
}

static void json_hmap_put_node(json_hmap_t *, json_hnode_t *);

static void json_hmap_rehash(json_hmap_t *hmap, size_t new_cap) {
    json_hnode_t *old_nodes = hmap->nodes;
    size_t old_cap = hmap->cap;

    hmap->cap = new_cap;
    hmap->nodes = json_hnodes_alloc(hmap->cap);
    hmap->size = 0;

    for (size_t i = 0; i < old_cap; ++i) {
        if (old_nodes[i].hash) {
            old_nodes[i].index = old_nodes[i].hash % hmap->cap;
            old_nodes[i].steps = 0;

            json_hmap_put_node(hmap, &old_nodes[i]);
        }
    }

    json_fat_free(old_nodes);
}

static inline void json_hmap_alloc_slot(json_hmap_t *hmap) {
    if (++hmap->size > hmap->cap >> 1)
        json_hmap_rehash(hmap, hmap->cap << 1);
}

static inline void json_hmap_free_slot(json_hmap_t *hmap) {
    if (hmap->size-- < hmap->cap >> 2 && hmap->cap > hmap->min_cap)
        json_hmap_rehash(hmap, hmap->cap >> 1);
}

static void json_hmap_make(json_hmap_t *hmap, size_t init_cap) {
	json_vec_make(&hmap->vec, init_cap);

    hmap->size = 0;
    hmap->cap = hmap->min_cap = init_cap;
    hmap->nodes = json_hnodes_alloc(hmap->cap);
}

// for rehash + put
static void json_hmap_put_node(json_hmap_t *hmap, json_hnode_t *node) {
    size_t index = node->index;

    // find suitable bucket
    while (hmap->nodes[index].hash) {
        if (hmap->nodes[index].hash == node->hash) {
            // found matching bucket
            hmap->nodes[index].object = node->object;
            return;
        }

        index = (index + 1) % hmap->cap;
        ++node->steps;
    }

    // found empty bucket
    hmap->nodes[index] = *node;

    json_hmap_alloc_slot(hmap);
}

static json_hnode_t *json_hmap_get_node(json_hmap_t *hmap, json_hash_t hash) {
	size_t index = hash % hmap->cap;

	// iterate through hash chain until match or empty node is found
	while (hmap->nodes[index].hash) {
		if (hmap->nodes[index].hash == hash)
			return &hmap->nodes[index];

		index = (index + 1) % hmap->cap;
	}

	return NULL;
}

static void json_hmap_put(json_hmap_t *hmap, char *key, json_object_t *object) {
	json_hnode_t node;

	json_vec_push(&hmap->vec, key);

	node.object = object;
	node.hash = json_hash_str(key);
	node.index = node.hash % hmap->cap;

	json_hmap_put_node(hmap, &node);
}

static json_object_t *json_hmap_get(json_hmap_t *hmap, char *key) {
	json_hnode_t *node = json_hmap_get_node(hmap, json_hash_str(key));

    return node ? node->object : NULL;
}

// parsing =====================================================================

// for mapping escape sequences
#define JSON_ESCAPE_CHARACTERS_X\
	X('"', '\"')\
	X('\\', '\\')\
	X('/', '/')\
	X('b', '\b')\
	X('f', '\f')\
	X('n', '\n')\
	X('r', '\r')\
	X('t', '\t')

static bool json_is_whitespace(char ch) {
	switch (ch) {
	case 0x20:
	case 0x0A:
	case 0x0D:
	case 0x09:
		return true;
	default:
		return false;
	}
}

static inline bool json_is_digit(char ch) {
	return ch >= '0' && ch <= '9';
}

// skip whitespace to start of next token
static void json_next_token(json_ctx_t *ctx) {
	while (1) {
		if (!json_is_whitespace(ctx->text[ctx->index]))
			return;

		++ctx->index;
	}
}

// compare next token with passed token
static bool json_token_equals(
	json_ctx_t *ctx, const char *token, size_t length
) {
	for (size_t i = 0; i < length; ++i)
		if (ctx->text[ctx->index + i] != token[i])
			return false;

	return true;
}

// error if next token does not equal passed token, otherwise skip
static void json_expect_token(
	json_ctx_t *ctx, const char *token, size_t length
) {
	if (!json_token_equals(ctx, token, length))
		JSON_CTX_ERROR(ctx, "unknown token, expected \"%s\"/\n", token);

	ctx->index += length;
}

// error if next character is not a valid json string character, otherwise
// return char and skip
static char json_expect_str_char(json_ctx_t *ctx) {
	if (ctx->text[ctx->index] == '\\') {
		// escape codes
		char ch;

		switch (ctx->text[++ctx->index]) {
#define X(a, b) case a: ch = b; break;
		JSON_ESCAPE_CHARACTERS_X
#undef X
		case 'u':
			JSON_CTX_ERROR(
				ctx,
				"ghh_json does not support unicode escape sequences currently."
				"\n"
			);
		default:
			JSON_CTX_ERROR(
				ctx,
				"unknown character escape: '%c' (%hhX)\n",
				ctx->text[ctx->index], ctx->text[ctx->index]
			);
		}

		++ctx->index;

		return ch;
	} else {
		// raw character
		return ctx->text[ctx->index++];
	}
}

// return string allocated on ctx allocator if valid string, otherwise error
static char *json_expect_string(json_ctx_t *ctx) {
	// verify string and count string length
	if (ctx->text[ctx->index++] != '\"')
		JSON_CTX_ERROR(ctx, "unknown token, expected string.\n");

	size_t start_index = ctx->index;
	size_t length = 0;

	while (ctx->text[ctx->index] != '\"') {
		switch (ctx->text[ctx->index]) {
		default:
			json_expect_str_char(ctx);
			++length;

			break;
		case '\n':
		case '\0':
			JSON_CTX_ERROR(ctx, "string ended unexpectedly.\n");
		}
	}

	// read string
	char *str = (char *)json_page_alloc(ctx->json, (length + 1) * sizeof(*str));

	ctx->index = start_index;

	for (size_t i = 0; i < length; ++i)
		str[i] = json_expect_str_char(ctx);

	str[length] = '\0';

	++ctx->index; // skip ending double quote

	return str;
}

static double json_expect_number(json_ctx_t *ctx) {
	// minus symbol
	bool negative = ctx->text[ctx->index] == '-';

	if (negative)
		++ctx->index;

	// integral component
	double num = 0.0;

	if (!json_is_digit(ctx->text[ctx->index]))
		JSON_CTX_ERROR(ctx, "expected digit.\n");

	while (json_is_digit(ctx->text[ctx->index])) {
		num *= 10.0;
		num += ctx->text[ctx->index++] - '0';
	}

	// fractional component
	if (ctx->text[ctx->index] == '.') {
		++ctx->index;

		if (!json_is_digit(ctx->text[ctx->index]))
			JSON_CTX_ERROR(ctx, "expected digit.\n");

		double fract = 0.0;

		while (json_is_digit(ctx->text[ctx->index])) {
			fract += ctx->text[ctx->index++] - '0';
			fract *= 0.1;
		}

		num += fract;
	}

	if (negative)
		num = -num;

	// exponential component
	if (ctx->text[ctx->index] == 'e' || ctx->text[ctx->index] == 'E') {
		++ctx->index;

		// read exponent
		bool neg_exp = false;
		if (ctx->text[ctx->index] == '+' || ctx->text[ctx->index] == '-')
			neg_exp = ctx->text[ctx->index++] == '-';

		if (!json_is_digit(ctx->text[ctx->index]))
			JSON_CTX_ERROR(ctx, "expected digit.\n");

		long exponent = 0;

		while (json_is_digit(ctx->text[ctx->index])) {
			exponent *= 10;
			exponent += ctx->text[ctx->index++] - '0';
		}

		// calculate
		if (neg_exp) {
			while (exponent--)
				num *= 0.1;
		} else {
			while (exponent--)
				num *= 10.0;
		}
	}

	return num;
}

static json_object_t *json_expect_obj(json_ctx_t *, json_object_t *);
static json_object_t *json_expect_array(json_ctx_t *, json_object_t *);

// fills object in with value
static void json_expect_value(json_ctx_t *ctx, json_object_t *object) {
	switch (ctx->text[ctx->index]) {
	case '{':
		json_expect_obj(ctx, object);
		object->type = JSON_OBJECT;

		break;
	case '[':
		json_expect_array(ctx, object);
		object->type = JSON_ARRAY;

		break;
	case '"':
		object->data.string = json_expect_string(ctx);
		object->type = JSON_STRING;

		break;
	case 't':
		json_expect_token(ctx, "true", 4);
		object->type = JSON_TRUE;

		break;
	case 'f':
		json_expect_token(ctx, "false", 5);
		object->type = JSON_FALSE;

		break;
	case 'n':
		json_expect_token(ctx, "null", 4);
		object->type = JSON_NULL;

		break;
	default:;
		// could be number
		if (json_is_digit(ctx->text[ctx->index])
		 || ctx->text[ctx->index] == '-') {
			object->data.number = json_expect_number(ctx);
			object->type = JSON_NUMBER;

			break;
		}

		JSON_CTX_ERROR(ctx, "unknown token, expected value.\n");
	}
}

static json_object_t *json_expect_array(
	json_ctx_t *ctx, json_object_t *object
) {
	json_vec_t *vec = (json_vec_t *)json_page_alloc(ctx->json, sizeof(*vec));
	json_vec_make(vec, 8);

	object->data.vec = vec;

	++ctx->index; // skip '['

	// parse key/value pairs
	while (1) {
		// get child and store
		json_object_t *child = (json_object_t *)json_page_alloc(
			ctx->json,
			sizeof(*child)
		);

		json_next_token(ctx);
		json_expect_value(ctx, child);

		json_vec_push(vec, child);

		// iterate
		json_next_token(ctx);

		if (ctx->text[ctx->index] == ']') {
			++ctx->index;
			break;
		}

		json_expect_token(ctx, ",", 1);
	}

	return object;
}

static json_object_t *json_expect_obj(json_ctx_t *ctx, json_object_t *object) {
	json_hmap_t *hmap = (json_hmap_t *)json_page_alloc(
		ctx->json,
		sizeof(*hmap)
	);
	json_hmap_make(hmap, 8);

	object->data.hmap = hmap;

	++ctx->index; // skip '{'

	// parse key/value pairs
	while (1) {
		// get pair and store
		json_object_t *child = (json_object_t *)json_page_alloc(
			ctx->json,
			sizeof(*object)
		);

		json_next_token(ctx);
		char *key = json_expect_string(ctx);

		json_next_token(ctx);
		json_expect_token(ctx, ":", 1);
		json_next_token(ctx);
		json_expect_value(ctx, child);

		json_hmap_put(hmap, key, child);

		// iterate
		json_next_token(ctx);

		if (ctx->text[ctx->index] == '}') {
			++ctx->index;
			break;
		}

		json_expect_token(ctx, ",", 1);
	}

	return object;
}

static void json_parse(json_t *json, const char *text) {
	json_ctx_t ctx;

	ctx.json = json;
	ctx.text = text;
	ctx.index = 0;

	// recursive parse at root
	json_next_token(&ctx);

	switch (ctx.text[ctx.index]) {
	case '{':
		json->root = (json_object_t *)json_page_alloc(
			ctx.json,
			sizeof(*json->root)
		);

		json_expect_obj(&ctx, json->root);
		json->root->type = JSON_OBJECT;

		json_next_token(&ctx);
		json_expect_token(&ctx, "", 1);

		break;
	case '[':
		json->root = (json_object_t *)json_page_alloc(
			ctx.json,
			sizeof(*json->root)
		);

		json_expect_array(&ctx, json->root);
		json->root->type = JSON_ARRAY;

		json_next_token(&ctx);
		json_expect_token(&ctx, "", 1);

		break;
	case '\0': // empty json is still valid json
		json->root = NULL;

		break;
	default:
		JSON_CTX_ERROR(&ctx, "invalid json root.\n");
	}
}

// lifetime api ================================================================

// internal json_t initializer
static void json_make(json_t *json) {
	// no ZII in cpp :(
	json->root = NULL;

	json->cur_page = json->used = 0;
	json->page_cap = JSON_INIT_PAGE_CAP;
	json->pages = (char **)json_fat_alloc(
		JSON_INIT_PAGE_CAP * sizeof(*json->pages)
	);

	json->pages[0] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);
}

static void json_parse(json_t *json, const char *text);

void json_load(json_t *json, char *text) {
	json_make(json);
	json_parse(json, text);
}

void json_load_file(json_t *json, const char *filepath) {
	JSON_DEBUG("reading\n");

    // open and check for existance
    FILE *file = fopen(filepath, "r");

    if (!file)
        JSON_ERROR("could not open file: \"%s\"\n", filepath);

    // read text through buffer
    char *text = NULL;
    char *buffer = (char *)JSON_MALLOC(JSON_FREAD_BUF_SIZE);
    size_t text_len = 0;
    bool done = false;

    do {
        size_t num_read = fread(
			buffer,
			sizeof(*buffer),
			JSON_FREAD_BUF_SIZE,
			file
		);

        if (num_read != JSON_FREAD_BUF_SIZE) {
            done = true;
            ++num_read;
        }

        // copy buffer into doc->text
        size_t last_len = text_len;

        text_len += num_read;
        text = (char *)json_fat_realloc(text, text_len);

        memcpy(text + last_len, buffer, num_read * sizeof(*buffer));
    } while (!done);

    text[text_len - 1] = '\0';

	free(buffer);

	// load and cleanup
	JSON_DEBUG("loading\n");

	json_load(json, text);

	json_fat_free(text);
    fclose(file);
}

static void json_free_object(json_object_t *);

static void json_free_array(json_object_t *object) {
	json_vec_t *vec = object->data.vec;

	for (size_t i = 0; i < vec->size; ++i) {
		json_object_t *child = vec->data[i];

		if (child->type == JSON_ARRAY)
			json_free_array(child);
		else if (child->type == JSON_OBJECT)
			json_free_object(child);
	}

	json_fat_free(vec->data);
}

static void json_free_object(json_object_t *object) {
	json_hmap_t *hmap = object->data.hmap;

	for (size_t i = 0; i < hmap->cap; ++i) {
		json_hnode_t *node = &hmap->nodes[i];

		if (node->hash) {
			if (node->object->type == JSON_ARRAY)
				json_free_array(node->object);
			else if (node->object->type == JSON_OBJECT)
				json_free_object(node->object);
		}
	}

	json_fat_free(hmap->vec.data);
	json_fat_free(hmap->nodes);
}

// recursively free object hashmap and array vectors
void json_unload(json_t *json) {
	if (json->root) {
		if (json->root->type == JSON_OBJECT)
			json_free_object(json->root);
		else
			json_free_array(json->root);
	}

	for (size_t i = 0; i <= json->cur_page; ++i)
		JSON_FREE(json->pages[i]);

	json_fat_free(json->pages);
}

// pretty print api ============================================================

static void json_fprint_level(FILE *file, int level) {
	fprintf(file, "%*s", level << 2, "");
}

// handles escape characters
// TODO json_escape_string?
static void json_fprint_string(FILE *file, char *str) {
	fprintf(file, "\"");

	while (*str) {
		switch (*str) {
#define X(a, b) case b: fprintf(file, "\\%c", a); break;
		JSON_ESCAPE_CHARACTERS_X
#undef X
		default:
			fprintf(file, "%c", *str);
			break;
		}

		++str;
	}

	fprintf(file, "\"");
}

static void json_fprint_array(FILE *file, json_object_t *object, int level);
static void json_fprint_obj(FILE *file, json_object_t *object, int level);

static void json_fprint_value(FILE *file, json_object_t *object, int level) {
	switch (object->type) {
	case JSON_OBJECT:
		json_fprint_obj(file, object, level);

		break;
	case JSON_ARRAY:
		json_fprint_array(file, object, level);

		break;
	case JSON_STRING:
		json_fprint_string(file, object->data.string);

		break;
	case JSON_NUMBER:
		if ((long)object->data.number == object->data.number)
			fprintf(file, "%ld", (long)object->data.number);
		else
			fprintf(file, "%lf", object->data.number);

		break;
	case JSON_TRUE:
		fprintf(file, "true");

		break;
	case JSON_FALSE:
		fprintf(file, "false");

		break;
	case JSON_NULL:
		fprintf(file, "null");

		break;
	}
}

static void json_fprint_array(FILE *file, json_object_t *object, int level) {
	fprintf(file, "[\n");

	++level;

	for (size_t i = 0; i < object->data.vec->size; ++i) {
		if (i)
			fprintf(file, ",\n");

		json_fprint_level(file, level);
		json_fprint_value(file, object->data.vec->data[i], level);
	}

	fprintf(file, "\n");

	json_fprint_level(file, level - 1);
	fprintf(file, "]");
}

static void json_fprint_obj(FILE *file, json_object_t *object, int level) {
	fprintf(file, "{\n");

	++level;

	json_hmap_t *hmap = object->data.hmap;
	json_vec_t *vec = &hmap->vec;

	for (size_t i = 0; i < vec->size; ++i) {
		if (i)
			fprintf(file, ",\n");

		json_fprint_level(file, level);
		json_fprint_string(file, vec->data[i]);
		fprintf(file, ": ");

		json_fprint_value(file, json_hmap_get(hmap, vec->data[i]), level);
	}

	fprintf(file, "\n");

	json_fprint_level(file, level - 1);
	fprintf(file, "}");
}

void json_fprint(FILE *file, json_object_t *object) {
	JSON_ASSERT(object, "attempted to json_fprint a NULL object pointer.\n");

	json_fprint_value(file, object, 0);
	fprintf(file, "\n");
}

void json_print(json_object_t *object) {
	json_fprint(stdout, object);
}

// get api =====================================================================

#define JSON_ASSERT_PROPER_CAST(json_type)\
	JSON_ASSERT(\
		object->type == json_type,\
		"attempted to cast %s to %s.\n",\
		json_types[object->type], json_types[json_type]\
	)

json_object_t *json_get_object(json_object_t *object, char *key) {
	JSON_ASSERT(
		object->type == JSON_OBJECT,
		"attempted to get child \"%s\" from a non-object.\n",
		key
	);

	return json_hmap_get(object->data.hmap, key);
}

json_object_t **json_get_array(
	json_object_t *object, char *key, size_t *out_size
) {
	return json_to_array(json_get_object(object, key), out_size);
}

char *json_get_string(json_object_t *object, char *key) {
	return json_to_string(json_get_object(object, key));
}

double json_get_number(json_object_t *object, char *key) {
	return json_to_number(json_get_object(object, key));
}

bool json_get_bool(json_object_t *object, char *key) {
	return json_to_bool(json_get_object(object, key));
}

json_object_t **json_to_array(json_object_t *object, size_t *out_size) {
	JSON_ASSERT_PROPER_CAST(JSON_ARRAY);

	json_vec_t *vec = object->data.vec;

	if (out_size)
		*out_size = vec->size;

	return (json_object_t **)vec->data;
}

char *json_to_string(json_object_t *object) {
	JSON_ASSERT_PROPER_CAST(JSON_STRING);

	return object->data.string;
}

double json_to_number(json_object_t *object) {
	JSON_ASSERT_PROPER_CAST(JSON_NUMBER);

	return object->data.number;
}

bool json_to_bool(json_object_t *object) {
	JSON_ASSERT(
		object->type == JSON_TRUE || object->type == JSON_FALSE,
		"attempted to cast %s to bool.\n",
		json_types[object->type]
	);

	return object->type == JSON_TRUE;
}

#endif // GHH_JSON_IMPL

#ifdef __cplusplus
}
#endif

#endif // GHH_JSON_H

/*

The MIT License (MIT)

Copyright (c) 2021 garrisonhh / Garrison Hinson-Hasty

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the “Software”), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/
