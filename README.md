# ghh_json.h

a single-header ISO-C99 (and C++ compatible) json loader.

## why?

obviously this isn't the first json library written for C, so why would I write
this?

1. easy build

there are great json C libraries with clean APIs and many more features than my
implementation out there, but they require build system integration which
immediately adds work I don't want to deal with.

2. easy usage

there are other single-header json libraries for C out there, but I wasn't super
into the way they are structured. I wrote this the way I would expect a json
library to behave. this is more of a taste thing than anything, if you don't
like how ghh_json works check out another implementation.

## usage

**note: ghh_json does not currently support unicode escape sequences, like**
**"\\u0123". otherwise it is json specification conforming.**

to include in your project:

```c
#define GHH_JSON_IMPL
#include "ghh_json.h"
```

the basic program structure looks like this:

```c
json_t json;
json_load_file(&json, "mydata.json");

// do stuff ...

json_unload(&json);
json_load(&json, "{ \"raw\": \"json data.\"}");

// do stuff...

json_unload(&json);
```

there are only 3 types you need to think about:
- `json_t`, a reusable memory context for json objects
  - access root element through `.root`.
- `json_type_e`, json type enum
- types are: `JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_TRUE,
  JSON_FALSE, JSON_NULL`
- `json_object_t`, a tagged union
  - access type through `.type`
  - access data using `json_get` and `json_to` functions

## api

### settings

```c
// define your own allocation functions
#define JSON_MALLOC(size)
#define JSON_FREE(size)

// change the size of the char buffer for fread()
#define JSON_FREAD_BUF_SIZE

// the json_t allocator works by allocating pages to accommodate objects and
// data. increasing this means less allocations during parsing
#define JSON_PAGE_SIZE
```

### json_t lifetime

```c
// load json from a string
void json_load(json_t *, char *text);
// create an empty json_t context
void json_load_empty(json_t *);
// load json from a file
void json_load_file(json_t *, const char *filepath);
// free all memory associated with json context
void json_unload(json_t *);
```

### data access

```c
// returns a string allocated with JSON_MALLOC
// if mini, won't add newlines or indentation
char *json_serialize(json_object_t *, bool mini, int indent, size_t *out_len);

// retrieve a key from an object
// if NDEBUG is not defined, will type check the root object
json_object_t *json_get_object(json_object_t *, char *key);
// returns actual, mutable array pointer
json_object_t **json_get_array(json_object_t *, char *key, size_t *out_size);
char *json_get_string(json_object_t *, char *key);
double json_get_number(json_object_t *, char *key);
bool json_get_bool(json_object_t *, char *key);

// cast an object to a type
// if NDEBUG is not defined, will type check the object
json_object_t **json_to_array(json_object_t *, size_t *out_size);
char *json_to_string(json_object_t *);
double json_to_number(json_object_t *);
bool json_to_bool(json_object_t *);
```

### data modification

```c
// remove a json_object from another json_object (unordered)
json_object_t *json_pop(json_object_t *, char *key);
// pop but ordered, this is O(n) rather than O(1) removal time
json_object_t *json_pop_ordered(json_object_t *, char *key);

// add a json_object to another json_object
void json_put(json_object_t *, char *key, json_object_t *child);

// create a new json type on a json_t, and add it to an object
json_object_t *json_put_object(json_t *, json_object_t *, char *key);
void json_put_array(
	json_t *, json_object_t *, json_object_t **objects, size_t size
);
void json_put_string(json_t *, json_object_t *, char *key, char *string);
void json_put_number(json_t *, json_object_t *, char *key, double number);
void json_put_bool(json_t *, json_object_t *, bool value);
void json_put_null(json_t *, json_object_t *, char *key);

// create a json data type on a json_t memory context
json_object_t *json_new_object(json_t *);
json_object_t *json_new_array(json_t *, json_object_t **objects, size_t size);
json_object_t *json_new_string(json_t *, char *string);
json_object_t *json_new_number(json_t *, double number);
json_object_t *json_new_bool(json_t *, bool value);
json_object_t *json_new_null(json_t *);
```
