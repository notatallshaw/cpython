/* PEP 440 versions with an inline compact key and separate full components. */
#define PY_SSIZE_T_CLEAN
#include "Python.h"

#include <stdint.h>
#include <string.h>

enum { PRE_A, PRE_B, PRE_RC, PRE_NONE = 0xff };
enum {
    PART_EPOCH,
    PART_RELEASE,
    PART_PRE,
    PART_POST,
    PART_DEV,
    PART_LOCAL,
    PART_COUNT
};

static const char *const PRE_NAMES[] = {"a", "b", "rc"};
static const char *const PART_NAMES[] = {"epoch", "release", "pre",
                                         "post",  "dev",     "local"};

typedef struct {
    uint64_t high;
    uint64_t low;
} VersionKey;

typedef struct {
    PyObject *parts[PART_COUNT];
    PyObject *key;
} VersionFull;

typedef struct {
    PyObject_HEAD
    Py_hash_t hash;
    /* Zero means full; compact releases contain 1-4 components. */
    uint8_t release_len;
    union {
        VersionKey key;
        VersionFull *full;
    } value;
} VersionObject;

typedef struct {
    PyObject *version_type;
    PyObject *invalid_version;
    PyObject *stable_suffix;
} VersionState;

typedef struct {
    uint16_t release[4];
    uint8_t release_count;
    uint8_t pre_letter;
    uint16_t pre_n;
    uint16_t post_n;
    uint16_t dev_n;
    uint8_t flags;
} SmallParts;

typedef struct {
    const char *s;
    Py_ssize_t len;
    Py_ssize_t val;
    int too_big;
    int present;
} NumberField;

typedef struct {
    int has_epoch;
    NumberField epoch;
    Py_ssize_t rel_count;
    const char *rel_span;
    Py_ssize_t rel_span_len;
    uint16_t rel_small[4];
    int rel_all_small;
    int has_pre;
    int pre_letter;
    NumberField pre_n;
    int has_post;
    NumberField post_n;
    int has_dev;
    NumberField dev_n;
    int has_local;
    const char *local_span;
    Py_ssize_t local_span_len;
} ParsedVersion;

static struct PyModuleDef version_module;

static inline VersionObject *
version_cast(PyObject *object)
{
    return (VersionObject *)object;
}

static inline int
is_small(const VersionObject *version)
{
    return version->release_len != 0;
}

static VersionState *
state_from_type(PyTypeObject *type)
{
    PyObject *module = PyType_GetModuleByDef(type, &version_module);
    if (module == NULL) {
        return NULL;
    }
    return PyModule_GetState(module);
}

static void
clear_parts(PyObject **parts)
{
    for (int i = 0; i < PART_COUNT; i++) {
        Py_XDECREF(parts[i]);
    }
}

/* Four stack entries cover the compact release width.
 * Longer tuples use heap storage. */
typedef struct {
    PyObject **items;
    Py_ssize_t length;
    PyObject *stack[4];
} TupleBuilder;

static int
tuple_builder_init(TupleBuilder *builder, Py_ssize_t length)
{
    memset(builder, 0, sizeof(*builder));
    builder->length = length;
    if (length <=
        (Py_ssize_t)(sizeof(builder->stack) / sizeof(builder->stack[0]))) {
        builder->items = builder->stack;
    }
    else {
        builder->items = PyMem_Calloc((size_t)length, sizeof(PyObject *));
        if (builder->items == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }
    return 0;
}

static void
tuple_builder_clear(TupleBuilder *builder)
{
    for (Py_ssize_t i = 0; i < builder->length; i++) {
        Py_XDECREF(builder->items[i]);
    }
    if (builder->items != builder->stack) {
        PyMem_Free(builder->items);
    }
}

static PyObject *
tuple_builder_finish(TupleBuilder *builder)
{
    PyObject *tuple = PyTuple_FromArray(builder->items, builder->length);
    tuple_builder_clear(builder);
    return tuple;
}

static inline uint32_t
small_pre(const VersionObject *version)
{
    return (uint32_t)((version->value.key.low >> 34) & 0xfffff);
}

static inline uint32_t
small_post(const VersionObject *version)
{
    return (uint32_t)((version->value.key.low >> 17) & 0x1ffff);
}

static inline uint32_t
small_dev(const VersionObject *version)
{
    return (uint32_t)(version->value.key.low & 0x1ffff);
}

static inline uint64_t
small_release(const VersionObject *version)
{
    return (version->value.key.high << 10) | (version->value.key.low >> 54);
}

static VersionKey
pack_small_key(const SmallParts *parts)
{
    uint64_t release = 0;
    for (int i = 0; i < parts->release_count; i++) {
        release |= (uint64_t)parts->release[i] << (48 - 16 * i);
    }
    uint32_t pre;
    if (parts->pre_letter != PRE_NONE) {
        pre = (1u << 18) | ((uint32_t)parts->pre_letter << 16) | parts->pre_n;
    }
    else if ((parts->flags & 2) && !(parts->flags & 1)) {
        pre = 0;
    }
    else {
        pre = 2u << 18;
    }
    uint32_t post = (parts->flags & 1) ? (1u << 16) | parts->post_n : 0;
    uint32_t dev = (parts->flags & 2) ? parts->dev_n : 0x1ffff;
    /* The 118 ordering bits span two standard C integers. */
    return (VersionKey){
        .high = release >> 10,
        .low = (release << 54) | ((uint64_t)pre << 34) |
               ((uint64_t)post << 17) | dev,
    };
}

static SmallParts
unpack_small(const VersionObject *version)
{
    uint64_t release = small_release(version);
    uint32_t pre = small_pre(version);
    uint32_t post = small_post(version);
    uint32_t dev = small_dev(version);
    SmallParts parts = {
        .release_count = version->release_len,
        .pre_letter = (pre >> 18) == 1 ? (pre >> 16) & 3 : PRE_NONE,
        .pre_n = (pre >> 18) == 1 ? pre & UINT16_MAX : 0,
        .post_n = (post >> 16) ? post & UINT16_MAX : 0,
        .dev_n = dev != 0x1ffff ? dev : 0,
        .flags = ((post >> 16) ? 1 : 0) | (dev != 0x1ffff ? 2 : 0),
    };
    for (int i = 0; i < parts.release_count; i++) {
        parts.release[i] = (uint16_t)(release >> (48 - 16 * i));
    }
    return parts;
}

static PyObject *
make_small_parts(PyTypeObject *type, const SmallParts *parts)
{
    VersionObject *version = (VersionObject *)type->tp_alloc(type, 0);
    if (version == NULL) {
        return NULL;
    }
    _Py_atomic_store_ssize_relaxed(&version->hash, -1);
    version->value.key = pack_small_key(parts);
    version->release_len = parts->release_count;
    return (PyObject *)version;
}

static PyObject *
make_full_parts(PyTypeObject *type, PyObject *const *parts)
{
    VersionFull *full = PyMem_Calloc(1, sizeof(*full));
    if (full == NULL) {
        return PyErr_NoMemory();
    }
    for (int i = 0; i < PART_COUNT; i++) {
        full->parts[i] = Py_NewRef(parts[i]);
    }
    VersionObject *version = (VersionObject *)type->tp_alloc(type, 0);
    if (version == NULL) {
        clear_parts(full->parts);
        PyMem_Free(full);
        return NULL;
    }
    /* No allocation or Python callback occurs before the object is complete.
     */
    _Py_atomic_store_ssize_relaxed(&version->hash, -1);
    version->value.full = full;
    version->release_len = 0;
    return (PyObject *)version;
}

static PyObject *
long_from(const char *s, Py_ssize_t len)
{
    char stackbuf[32];
    char *buf = (len < (Py_ssize_t)sizeof(stackbuf)) ? stackbuf
                                                     : PyMem_Malloc(len + 1);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    PyObject *r = PyLong_FromString(buf, NULL, 10);
    if (buf != stackbuf) {
        PyMem_Free(buf);
    }
    return r;
}

static int
is_sep(char c)
{
    return c == '.' || c == '-' || c == '_';
}

static int
lc(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 'a';
    }
    return c;
}

/* case-insensitive match of kw at p; return length matched or 0 */
static Py_ssize_t
match_kw(const char *p, const char *end, const char *kw)
{
    Py_ssize_t i = 0;
    while (kw[i]) {
        if (p + i >= end || lc(p[i]) != kw[i]) {
            return 0;
        }
        i++;
    }
    return i;
}

/* read [0-9]+ into f; returns number of digits consumed (0 if none) */
static Py_ssize_t
read_digits(const char *p, const char *end, NumberField *f)
{
    const char *start = p;
    while (p < end && *p >= '0' && *p <= '9') {
        p++;
    }
    Py_ssize_t n = p - start;
    if (n == 0) {
        memset(f, 0, sizeof(*f));
        return 0;
    }
    f->present = 1;
    f->s = start;
    f->len = n;
    f->too_big = 0;
    Py_ssize_t v = 0;
    for (const char *q = start; q < p; q++) {
        if (v > 0xffff) {
            f->too_big = 1;
            break;
        }
        v = v * 10 + (*q - '0');
    }
    if (v > 0xffff) {
        f->too_big = 1;
    }
    f->val = v;
    return n;
}

static int
match_pre_letter(const char *p, const char *end, int *letter, Py_ssize_t *mlen)
{
    Py_ssize_t m;
    if ((m = match_kw(p, end, "alpha"))) {
        *letter = PRE_A;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "beta"))) {
        *letter = PRE_B;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "preview"))) {
        *letter = PRE_RC;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "pre"))) {
        *letter = PRE_RC;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "rc"))) {
        *letter = PRE_RC;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "a"))) {
        *letter = PRE_A;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "b"))) {
        *letter = PRE_B;
        *mlen = m;
        return 1;
    }
    if ((m = match_kw(p, end, "c"))) {
        *letter = PRE_RC;
        *mlen = m;
        return 1;
    }
    return 0;
}

/* returns 0 on success, -1 on invalid version */
static int
parse_version(const char *s, Py_ssize_t n, ParsedVersion *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = s;
    const char *end = s + n;

    while (p < end && Py_UNICODE_ISSPACE((unsigned char)*p)) {
        p++;
    }
    while (end > p && Py_UNICODE_ISSPACE((unsigned char)end[-1])) {
        end--;
    }

    if (p < end && (lc(*p) == 'v')) {
        p++;
    }

    /* epoch vs first release segment: digits then '!' => epoch */
    {
        const char *d = p;
        while (d < end && *d >= '0' && *d <= '9') {
            d++;
        }
        if (d > p && d < end && *d == '!') {
            NumberField f;
            read_digits(p, end, &f);
            out->has_epoch = 1;
            out->epoch = f;
            p = d + 1;
        }
    }

    /* release: [0-9]+ (. [0-9]+)* */
    if (!(p < end && *p >= '0' && *p <= '9')) {
        return -1;
    }
    out->rel_span = p;
    out->rel_all_small = 1;
    out->rel_count = 0;
    for (;;) {
        NumberField f;
        Py_ssize_t got = read_digits(p, end, &f);
        if (got == 0) {
            return -1;
        }
        if (out->rel_count < 4) {
            out->rel_small[out->rel_count] = (uint16_t)f.val;
        }
        if (f.too_big) {
            out->rel_all_small = 0;
        }
        out->rel_count++;
        p += got;
        if (p < end && *p == '.') {
            /* lookahead: only consume '.' if a digit follows (else it may be
               a pre/post/dev separator) */
            if (p + 1 < end && p[1] >= '0' && p[1] <= '9') {
                p++;
                continue;
            }
        }
        break;
    }
    out->rel_span_len = p - out->rel_span;

    /* pre: [sep]? letter [sep]? [N]? */
    {
        const char *save = p;
        const char *q = p;
        if (q < end && is_sep(*q)) {
            q++;
        }
        int letter;
        Py_ssize_t mlen;
        if (q < end && match_pre_letter(q, end, &letter, &mlen)) {
            q += mlen;
            if (q < end && is_sep(*q)) {
                q++;
            }
            NumberField f;
            q += read_digits(q, end, &f);
            out->has_pre = 1;
            out->pre_letter = letter;
            out->pre_n = f; /* present flag inside */
            if (!f.present) {
                out->pre_n.val = 0;
                out->pre_n.too_big = 0;
            }
            p = q;
        }
        else {
            p = save;
        }
    }

    /* post: '-' N  |  [sep]? (post|rev|r) [sep]? [N]? */
    {
        const char *save = p;
        if (p < end && *p == '-' && p + 1 < end && p[1] >= '0' &&
            p[1] <= '9') {
            p++;
            NumberField f;
            p += read_digits(p, end, &f);
            out->has_post = 1;
            out->post_n = f;
        }
        else {
            const char *q = p;
            if (q < end && is_sep(*q)) {
                q++;
            }
            Py_ssize_t m;
            Py_ssize_t klen = 0;
            if ((m = match_kw(q, end, "post"))) {
                klen = m;
            }
            else if ((m = match_kw(q, end, "rev"))) {
                klen = m;
            }
            else if ((m = match_kw(q, end, "r"))) {
                klen = m;
            }
            if (klen) {
                q += klen;
                if (q < end && is_sep(*q)) {
                    q++;
                }
                NumberField f;
                q += read_digits(q, end, &f);
                out->has_post = 1;
                if (!f.present) {
                    f.val = 0;
                    f.too_big = 0;
                }
                out->post_n = f;
                p = q;
            }
            else {
                p = save;
            }
        }
    }

    /* dev: [sep]? dev [sep]? [N]? */
    {
        const char *save = p;
        const char *q = p;
        if (q < end && is_sep(*q)) {
            q++;
        }
        Py_ssize_t m = match_kw(q, end, "dev");
        if (m) {
            q += m;
            if (q < end && is_sep(*q)) {
                q++;
            }
            NumberField f;
            q += read_digits(q, end, &f);
            out->has_dev = 1;
            if (!f.present) {
                f.val = 0;
                f.too_big = 0;
            }
            out->dev_n = f;
            p = q;
        }
        else {
            p = save;
        }
    }

    /* local: + [a-z0-9]+ ([sep][a-z0-9]+)* */
    if (p < end && *p == '+') {
        p++;
        out->has_local = 1;
        out->local_span = p;
        int seg_chars = 0;
        for (;;) {
            if (p < end && ((lc(*p) >= 'a' && lc(*p) <= 'z') ||
                            (*p >= '0' && *p <= '9'))) {
                seg_chars = 1;
                p++;
            }
            else if (p < end && is_sep(*p) && seg_chars > 0) {
                seg_chars = 0;
                p++;
            }
            else {
                break;
            }
        }
        if (seg_chars == 0) {
            return -1; /* empty trailing segment or empty local */
        }
        out->local_span_len = p - out->local_span;
    }

    if (p != end) {
        return -1;
    }
    return 0;
}

static PyObject *
local_from_ascii(const char *text, Py_ssize_t length)
{
    Py_ssize_t count = 1;
    for (Py_ssize_t i = 0; i < length; i++) {
        count += is_sep(text[i]);
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, count) < 0) {
        return NULL;
    }
    Py_ssize_t start = 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        Py_ssize_t end = start;
        int numeric = 1;
        while (end < length && !is_sep(text[end])) {
            if (text[end] < '0' || text[end] > '9') {
                numeric = 0;
            }
            end++;
        }
        PyObject *part;
        if (numeric) {
            part = long_from(text + start, end - start);
        }
        else {
            part = PyUnicode_New(end - start, 127);
            if (part != NULL) {
                Py_UCS1 *data = PyUnicode_1BYTE_DATA(part);
                for (Py_ssize_t j = start; j < end; j++) {
                    data[j - start] = (Py_UCS1)lc(text[j]);
                }
            }
        }
        if (part == NULL) {
            tuple_builder_clear(&builder);
            return NULL;
        }
        builder.items[i] = part;
        start = end + 1;
    }
    return tuple_builder_finish(&builder);
}

static PyObject *
normalize_local(PyObject *value, VersionState *state)
{
    if (value == Py_None) {
        return Py_NewRef(Py_None);
    }
    if (PyUnicode_Check(value) && PyUnicode_IS_ASCII(value)) {
        Py_ssize_t length;
        const char *text = PyUnicode_AsUTF8AndSize(value, &length);
        if (text == NULL) {
            return NULL;
        }
        int empty = 1;
        Py_ssize_t i;
        for (i = 0; i < length; i++) {
            char c = text[i];
            if (is_sep(c)) {
                if (empty) {
                    break;
                }
                empty = 1;
            }
            else if ((c >= '0' && c <= '9') ||
                     (lc(c) >= 'a' && lc(c) <= 'z')) {
                empty = 0;
            }
            else {
                break;
            }
        }
        if (i == length && !empty) {
            return local_from_ascii(text, length);
        }
    }
    PyErr_Format(state->invalid_version,
                 "local must be a valid version string, got %R", value);
    return NULL;
}

static int
parts_from_parsed(const ParsedVersion *parsed, PyObject **parts)
{
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] = Py_NewRef(Py_None);
    }
    Py_SETREF(parts[PART_EPOCH],
              parsed->has_epoch ? long_from(parsed->epoch.s, parsed->epoch.len)
                                : PyLong_FromLong(0));
    if (parts[PART_EPOCH] == NULL) {
        return -1;
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, parsed->rel_count) < 0) {
        return -1;
    }
    const char *cursor = parsed->rel_span;
    const char *end = cursor + parsed->rel_span_len;
    for (Py_ssize_t i = 0; i < parsed->rel_count; i++) {
        const char *start = cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
        PyObject *part = long_from(start, cursor - start);
        if (part == NULL) {
            tuple_builder_clear(&builder);
            return -1;
        }
        builder.items[i] = part;
        if (cursor < end) {
            cursor++;
        }
    }
    Py_SETREF(parts[PART_RELEASE], tuple_builder_finish(&builder));
    if (parts[PART_RELEASE] == NULL) {
        return -1;
    }
    if (parsed->has_pre) {
        PyObject *number = parsed->pre_n.present
                               ? long_from(parsed->pre_n.s, parsed->pre_n.len)
                               : PyLong_FromLong(0);
        if (number == NULL) {
            return -1;
        }
        Py_SETREF(
            parts[PART_PRE],
            Py_BuildValue("(sN)", PRE_NAMES[parsed->pre_letter], number));
        if (parts[PART_PRE] == NULL) {
            return -1;
        }
    }
    if (parsed->has_post) {
        Py_SETREF(parts[PART_POST],
                  parsed->post_n.present
                      ? long_from(parsed->post_n.s, parsed->post_n.len)
                      : PyLong_FromLong(0));
        if (parts[PART_POST] == NULL) {
            return -1;
        }
    }
    if (parsed->has_dev) {
        Py_SETREF(parts[PART_DEV],
                  parsed->dev_n.present
                      ? long_from(parsed->dev_n.s, parsed->dev_n.len)
                      : PyLong_FromLong(0));
        if (parts[PART_DEV] == NULL) {
            return -1;
        }
    }
    if (parsed->has_local) {
        Py_SETREF(parts[PART_LOCAL], local_from_ascii(parsed->local_span,
                                                      parsed->local_span_len));
        if (parts[PART_LOCAL] == NULL) {
            return -1;
        }
    }
    return 0;
}

static PyObject *
version_new(PyTypeObject *type, PyObject *args, PyObject *keywords)
{
    static char *names[] = {"version", NULL};
    PyObject *original;
    if (!PyArg_ParseTupleAndKeywords(args, keywords, "O:Version", names,
                                     &original)) {
        return NULL;
    }
    PyObject *text = NULL;
    if (!PyUnicode_Check(original)) {
        goto invalid;
    }
    if (PyUnicode_IS_ASCII(original)) {
        text = Py_NewRef(original);
    }
    else {
        Py_ssize_t start = 0;
        Py_ssize_t end = PyUnicode_GET_LENGTH(original);
        while (start < end &&
               Py_UNICODE_ISSPACE(PyUnicode_READ_CHAR(original, start))) {
            start++;
        }
        while (end > start &&
               Py_UNICODE_ISSPACE(PyUnicode_READ_CHAR(original, end - 1))) {
            end--;
        }
        text = PyUnicode_Substring(original, start, end);
        if (text == NULL) {
            return NULL;
        }
        if (!PyUnicode_IS_ASCII(text)) {
            goto invalid;
        }
    }
    Py_ssize_t length;
    const char *data = PyUnicode_AsUTF8AndSize(text, &length);
    if (data == NULL) {
        Py_DECREF(text);
        return NULL;
    }
    ParsedVersion parsed;
    if (parse_version(data, length, &parsed) < 0) {
        goto invalid;
    }
    int small = (!parsed.has_epoch ||
                 (!parsed.epoch.too_big && parsed.epoch.val == 0)) &&
                !parsed.has_local && parsed.rel_count <= 4 &&
                parsed.rel_all_small &&
                (!parsed.has_pre || !parsed.pre_n.too_big) &&
                (!parsed.has_post || !parsed.post_n.too_big) &&
                (!parsed.has_dev || !parsed.dev_n.too_big);
    PyObject *result;
    if (small) {
        SmallParts parts = {
            .release_count = (uint8_t)parsed.rel_count,
            .pre_letter = parsed.has_pre ? parsed.pre_letter : PRE_NONE,
            .pre_n = parsed.has_pre ? (uint16_t)parsed.pre_n.val : 0,
            .post_n = parsed.has_post ? (uint16_t)parsed.post_n.val : 0,
            .dev_n = parsed.has_dev ? (uint16_t)parsed.dev_n.val : 0,
            .flags = parsed.has_post | (parsed.has_dev << 1),
        };
        memcpy(parts.release, parsed.rel_small, sizeof(parts.release));
        result = make_small_parts(type, &parts);
    }
    else {
        PyObject *parts[PART_COUNT] = {0};
        result = parts_from_parsed(&parsed, parts) < 0
                     ? NULL
                     : make_full_parts(type, parts);
        clear_parts(parts);
    }
    Py_DECREF(text);
    return result;

invalid:
    Py_XDECREF(text);
    VersionState *state = state_from_type(type);
    if (state != NULL) {
        PyErr_Format(state->invalid_version, "Invalid version: %R", original);
    }
    return NULL;
}

static int
nonnegative_integer(PyObject *value)
{
    if (!PyLong_Check(value)) {
        return 0;
    }
    if (PyLong_CheckExact(value)) {
        return !PyLong_IsNegative(value);
    }
    return PyObject_RichCompareBool(
        value, Py_GetConstantBorrowed(Py_CONSTANT_ZERO), Py_GE);
}

static PyObject *
normalize_part(int index, PyObject *value, VersionState *st)
{
    if (index == PART_EPOCH) {
        int truth = PyObject_IsTrue(value);
        if (truth < 0) {
            return NULL;
        }
        if (!truth) {
            return PyLong_FromLong(0);
        }
        int valid = nonnegative_integer(value);
        if (valid < 0) {
            return NULL;
        }
        if (valid) {
            return Py_NewRef(value);
        }
        PyErr_Format(st->invalid_version,
                     "epoch must be non-negative integer, got %S", value);
    }
    else if (index == PART_RELEASE) {
        if (value == Py_None) {
            return Py_BuildValue("(i)", 0);
        }
        if (PyTuple_Check(value) && PyTuple_GET_SIZE(value) > 0) {
            Py_ssize_t i;
            for (i = 0; i < PyTuple_GET_SIZE(value); i++) {
                int valid = nonnegative_integer(PyTuple_GET_ITEM(value, i));
                if (valid < 0) {
                    return NULL;
                }
                if (!valid) {
                    break;
                }
            }
            if (i == PyTuple_GET_SIZE(value)) {
                return PyTuple_GetSlice(value, 0, PyTuple_GET_SIZE(value));
            }
        }
        PyErr_Format(st->invalid_version,
                     "release must be a non-empty tuple of non-negative "
                     "integers, got %S",
                     value);
    }
    else if (index == PART_PRE) {
        if (value == Py_None) {
            return Py_NewRef(Py_None);
        }
        if (PyTuple_Check(value) && PyTuple_GET_SIZE(value) == 2) {
            PyObject *letter = PyTuple_GET_ITEM(value, 0);
            PyObject *number = PyTuple_GET_ITEM(value, 1);
            if (PyUnicode_Check(letter) && PyUnicode_IS_ASCII(letter)) {
                Py_ssize_t length, matched;
                const char *text = PyUnicode_AsUTF8AndSize(letter, &length);
                if (!text) {
                    return NULL;
                }
                int kind;
                if (match_pre_letter(text, text + length, &kind, &matched) &&
                    matched == length) {
                    int valid = nonnegative_integer(number);
                    if (valid < 0) {
                        return NULL;
                    }
                    if (valid) {
                        return Py_BuildValue("(sO)", PRE_NAMES[kind], number);
                    }
                }
            }
        }
        PyErr_Format(
            st->invalid_version,
            "pre must be a tuple of ('a'|'b'|'rc', non-negative int), got %S",
            value);
    }
    else if (index == PART_POST || index == PART_DEV) {
        if (value == Py_None) {
            return Py_NewRef(Py_None);
        }
        int valid = nonnegative_integer(value);
        if (valid < 0) {
            return NULL;
        }
        if (valid) {
            return Py_NewRef(value);
        }
        PyErr_Format(st->invalid_version,
                     "%s must be non-negative integer, got %S",
                     PART_NAMES[index], value);
    }
    else {
        return normalize_local(value, st);
    }
    return NULL;
}

static int
parse_parts_arguments(PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwnames, const char *method, PyObject **parts)
{
    if (nargs != 0) {
        PyErr_Format(PyExc_TypeError, "%s() accepts keyword arguments only",
                     method);
        return -1;
    }
    Py_ssize_t count = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *name = PyTuple_GET_ITEM(kwnames, i);
        if (!PyUnicode_Check(name)) {
            PyErr_Format(PyExc_TypeError, "%s() keywords must be strings",
                         method);
            return -1;
        }
        int index;
        for (index = 0; index < PART_COUNT; index++) {
            if (PyUnicode_CompareWithASCIIString(name, PART_NAMES[index]) ==
                0) {
                break;
            }
        }
        if (index == PART_COUNT) {
            PyErr_Format(PyExc_TypeError,
                         "%s() got an unexpected keyword argument '%U'",
                         method, name);
            return -1;
        }
        if (parts[index]) {
            PyErr_Format(PyExc_TypeError, "%s() got multiple values for '%U'",
                         method, name);
            return -1;
        }
        parts[index] = args[i];
    }
    return 0;
}

static int
small_number(PyObject *value, uint16_t *result)
{
    /* Preserve int subclasses in FULL, including their public representation.
     */
    if (!PyLong_CheckExact(value)) {
        return 0;
    }
    int overflow;
    long number = PyLong_AsLongAndOverflow(value, &overflow);
    if (overflow || number > UINT16_MAX) {
        return 0;
    }
    assert(number >= 0);
    *result = (uint16_t)number;
    return 1;
}

static int
apply_small_parts(SmallParts *small, PyObject *const *parts)
{
    if (parts[PART_EPOCH] && (!PyLong_CheckExact(parts[PART_EPOCH]) ||
                              PyObject_IsTrue(parts[PART_EPOCH]))) {
        return 0;
    }
    if (parts[PART_LOCAL] && parts[PART_LOCAL] != Py_None) {
        return 0;
    }
    if (parts[PART_RELEASE]) {
        PyObject *release = parts[PART_RELEASE];
        Py_ssize_t count = PyTuple_GET_SIZE(release);
        if (!PyTuple_CheckExact(release) || count > 4) {
            return 0;
        }
        small->release_count = (uint8_t)count;
        for (Py_ssize_t i = 0; i < count; i++) {
            if (!small_number(PyTuple_GET_ITEM(release, i),
                              &small->release[i])) {
                return 0;
            }
        }
    }
    if (parts[PART_PRE]) {
        small->pre_letter = PRE_NONE;
        small->pre_n = 0;
        if (parts[PART_PRE] != Py_None) {
            PyObject *pre = parts[PART_PRE];
            if (!small_number(PyTuple_GET_ITEM(pre, 1), &small->pre_n)) {
                return 0;
            }
            PyObject *letter = PyTuple_GET_ITEM(pre, 0);
            small->pre_letter =
                PyUnicode_CompareWithASCIIString(letter, "a") == 0   ? PRE_A
                : PyUnicode_CompareWithASCIIString(letter, "b") == 0 ? PRE_B
                                                                     : PRE_RC;
        }
    }
    for (int index = PART_POST; index <= PART_DEV; index++) {
        if (!parts[index]) {
            continue;
        }
        int flag = index == PART_POST ? 1 : 2;
        uint16_t *number = index == PART_POST ? &small->post_n : &small->dev_n;
        *number = 0;
        small->flags &= ~flag;
        if (parts[index] != Py_None) {
            if (!small_number(parts[index], number)) {
                return 0;
            }
            small->flags |= flag;
        }
    }
    return 1;
}

static PyObject *
make_parts(PyTypeObject *type, PyObject *const *parts)
{
    SmallParts small = {0};
    if (apply_small_parts(&small, parts)) {
        return make_small_parts(type, &small);
    }
    return make_full_parts(type, parts);
}

static PyObject *
version_from_parts(PyObject *type, PyObject *const *args, Py_ssize_t nargs,
                   PyObject *kwnames)
{
    PyObject *arguments[PART_COUNT] = {0};
    if (parse_parts_arguments(args, nargs, kwnames, "from_parts", arguments) <
        0) {
        return NULL;
    }
    if (!arguments[PART_RELEASE]) {
        PyErr_SetString(
            PyExc_TypeError,
            "from_parts() missing required keyword argument: 'release'");
        return NULL;
    }
    VersionState *st = state_from_type((PyTypeObject *)type);
    if (st == NULL) {
        return NULL;
    }
    PyObject *parts[PART_COUNT] = {0};
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] =
            normalize_part(i, arguments[i] ? arguments[i] : Py_None, st);
        if (!parts[i]) {
            clear_parts(parts);
            return NULL;
        }
    }
    PyObject *result = make_parts((PyTypeObject *)type, parts);
    clear_parts(parts);
    return result;
}

static PyObject *
get_epoch(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        return PyLong_FromLong(0);
    }
    return Py_NewRef(version->value.full->parts[PART_EPOCH]);
}

static PyObject *
get_release(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_RELEASE]);
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, version->release_len) < 0) {
        return NULL;
    }
    uint64_t packed = small_release(version);
    for (int i = 0; i < version->release_len; i++) {
        PyObject *part =
            PyLong_FromUnsignedLong((packed >> (48 - 16 * i)) & UINT16_MAX);
        if (part == NULL) {
            tuple_builder_clear(&builder);
            return NULL;
        }
        builder.items[i] = part;
    }
    return tuple_builder_finish(&builder);
}

static PyObject *
get_pre(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_PRE]);
    }
    uint32_t pre = small_pre(version);
    if ((pre >> 18) != 1) {
        Py_RETURN_NONE;
    }
    return Py_BuildValue("(sI)", PRE_NAMES[(pre >> 16) & 3], pre & UINT16_MAX);
}

static PyObject *
get_post(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_POST]);
    }
    uint32_t post = small_post(version);
    if (!(post >> 16)) {
        Py_RETURN_NONE;
    }
    return PyLong_FromUnsignedLong(post & UINT16_MAX);
}

static PyObject *
get_dev(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_DEV]);
    }
    uint32_t dev = small_dev(version);
    if (dev == 0x1ffff) {
        Py_RETURN_NONE;
    }
    return PyLong_FromUnsignedLong(dev);
}

static int
write_local(PyUnicodeWriter *writer, PyObject *local)
{
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(local); i++) {
        if (i != 0 && PyUnicodeWriter_WriteChar(writer, '.') < 0) {
            return -1;
        }
        if (PyUnicodeWriter_WriteStr(writer, PyTuple_GET_ITEM(local, i)) < 0) {
            return -1;
        }
    }
    return 0;
}

static PyObject *
get_local(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        Py_RETURN_NONE;
    }
    PyObject *local = version->value.full->parts[PART_LOCAL];
    if (local == Py_None) {
        Py_RETURN_NONE;
    }
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }
    if (write_local(writer, local) < 0) {
        PyUnicodeWriter_Discard(writer);
        return NULL;
    }
    return PyUnicodeWriter_Finish(writer);
}

static PyObject *
get_is_prerelease(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        return PyBool_FromLong((small_pre(version) >> 18) == 1 ||
                               small_dev(version) != 0x1ffff);
    }
    PyObject **parts = version->value.full->parts;
    return PyBool_FromLong(parts[PART_PRE] != Py_None ||
                           parts[PART_DEV] != Py_None);
}

static PyObject *
get_is_postrelease(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    return PyBool_FromLong(
        is_small(version) ? small_post(version) >> 16
                          : version->value.full->parts[PART_POST] != Py_None);
}

static PyObject *
get_is_devrelease(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    return PyBool_FromLong(
        is_small(version) ? small_dev(version) != 0x1ffff
                          : version->value.full->parts[PART_DEV] != Py_None);
}

static PyObject *
release_item(PyObject *self, Py_ssize_t index)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        if (index >= version->release_len) {
            return PyLong_FromLong(0);
        }
        return PyLong_FromUnsignedLong(
            (small_release(version) >> (48 - 16 * index)) & UINT16_MAX);
    }
    PyObject *release = version->value.full->parts[PART_RELEASE];
    if (index >= PyTuple_GET_SIZE(release)) {
        return PyLong_FromLong(0);
    }
    return Py_NewRef(PyTuple_GET_ITEM(release, index));
}

static PyObject *
get_major(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 0);
}

static PyObject *
get_minor(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 1);
}

static PyObject *
get_micro(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 2);
}

enum { FORMAT_ALL, FORMAT_PUBLIC, FORMAT_BASE };

static PyObject *
format_version(PyObject *self, int mode)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        SmallParts parts = unpack_small(version);
        char buffer[128];
        int offset = 0;
        for (int i = 0; i < parts.release_count; i++) {
            offset += PyOS_snprintf(buffer + offset, sizeof(buffer) - offset,
                                    "%s%u", i ? "." : "", parts.release[i]);
        }
        if (mode != FORMAT_BASE) {
            if (parts.pre_letter != PRE_NONE) {
                offset += PyOS_snprintf(
                    buffer + offset, sizeof(buffer) - offset, "%s%u",
                    PRE_NAMES[parts.pre_letter], parts.pre_n);
            }
            if (parts.flags & 1) {
                offset +=
                    PyOS_snprintf(buffer + offset, sizeof(buffer) - offset,
                                  ".post%u", parts.post_n);
            }
            if (parts.flags & 2) {
                offset +=
                    PyOS_snprintf(buffer + offset, sizeof(buffer) - offset,
                                  ".dev%u", parts.dev_n);
            }
        }
        return PyUnicode_FromStringAndSize(buffer, offset);
    }

    PyObject **parts = version->value.full->parts;
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }
    int epoch = PyObject_IsTrue(parts[PART_EPOCH]);
    if (epoch < 0 || (epoch && PyUnicodeWriter_Format(
                                   writer, "%S!", parts[PART_EPOCH]) < 0)) {
        goto error;
    }
    PyObject *release = parts[PART_RELEASE];
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(release); i++) {
        if (PyUnicodeWriter_Format(writer, "%s%S", i ? "." : "",
                                   PyTuple_GET_ITEM(release, i)) < 0) {
            goto error;
        }
    }
    if (mode == FORMAT_BASE) {
        return PyUnicodeWriter_Finish(writer);
    }
    PyObject *pre = parts[PART_PRE];
    if (pre != Py_None &&
        PyUnicodeWriter_Format(writer, "%S%S", PyTuple_GET_ITEM(pre, 0),
                               PyTuple_GET_ITEM(pre, 1)) < 0) {
        goto error;
    }
    if (parts[PART_POST] != Py_None &&
        PyUnicodeWriter_Format(writer, ".post%S", parts[PART_POST]) < 0) {
        goto error;
    }
    if (parts[PART_DEV] != Py_None &&
        PyUnicodeWriter_Format(writer, ".dev%S", parts[PART_DEV]) < 0) {
        goto error;
    }
    if (mode == FORMAT_ALL && parts[PART_LOCAL] != Py_None) {
        if (PyUnicodeWriter_WriteChar(writer, '+') < 0 ||
            write_local(writer, parts[PART_LOCAL]) < 0) {
            goto error;
        }
    }
    return PyUnicodeWriter_Finish(writer);

error:
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static PyObject *
version_str(PyObject *self)
{
    return format_version(self, FORMAT_ALL);
}

static PyObject *
get_base_version(PyObject *self, void *Py_UNUSED(context))
{
    return format_version(self, FORMAT_BASE);
}

static PyObject *
get_public(PyObject *self, void *Py_UNUSED(context))
{
    return format_version(self, FORMAT_PUBLIC);
}

static PyObject *
version_repr(PyObject *self)
{
    PyObject *text = version_str(self);
    if (text == NULL) {
        return NULL;
    }
    PyObject *name = PyType_GetName(Py_TYPE(self));
    if (name == NULL) {
        Py_DECREF(text);
        return NULL;
    }
    PyObject *result = PyUnicode_FromFormat("<%U(%R)>", name, text);
    Py_DECREF(name);
    Py_DECREF(text);
    return result;
}

static PyObject *
version_sizeof(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    size_t size = (size_t)Py_TYPE(self)->tp_basicsize;
    if (!is_small(version_cast(self))) {
        size += sizeof(VersionFull);
    }
    return PyLong_FromSize_t(size);
}

static PyObject *
version_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *text = version_str(self);
    if (text == NULL) {
        return NULL;
    }
    PyObject *arguments = PyTuple_Pack(1, text);
    Py_DECREF(text);
    if (arguments == NULL) {
        return NULL;
    }
    PyObject *state = PyObject_CallMethod(self, "__getstate__", NULL);
    if (state == NULL) {
        Py_DECREF(arguments);
        return NULL;
    }
    PyObject *result = PyTuple_Pack(3, Py_TYPE(self), arguments, state);
    Py_DECREF(arguments);
    Py_DECREF(state);
    return result;
}

static int
parts_from_version(PyObject *self, PyObject **parts)
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        for (int i = 0; i < PART_COUNT; i++) {
            parts[i] = Py_NewRef(version->value.full->parts[i]);
        }
        return 0;
    }
    static const getter getters[PART_COUNT] = {
        get_epoch, get_release, get_pre, get_post, get_dev, get_local};
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] = getters[i](self, NULL);
        if (parts[i] == NULL) {
            return -1;
        }
    }
    return 0;
}

static PyObject *
version_replace(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
                PyObject *kwnames)
{
    PyObject *arguments[PART_COUNT] = {0};
    if (parse_parts_arguments(args, nargs, kwnames, "__replace__", arguments) <
        0) {
        return NULL;
    }
    if (!kwnames || !PyTuple_GET_SIZE(kwnames)) {
        return Py_NewRef(self);
    }

    VersionState *st = state_from_type(Py_TYPE(self));
    if (st == NULL) {
        return NULL;
    }
    PyObject *parts[PART_COUNT] = {0};
    for (int i = 0; i < PART_COUNT; i++) {
        if (arguments[i]) {
            parts[i] = normalize_part(i, arguments[i], st);
            if (!parts[i]) {
                clear_parts(parts);
                return NULL;
            }
        }
    }

    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        SmallParts small = unpack_small(version);
        if (apply_small_parts(&small, parts)) {
            VersionKey key = pack_small_key(&small);
            int unchanged = small.release_count == version->release_len &&
                            key.high == version->value.key.high &&
                            key.low == version->value.key.low;
            PyObject *result = unchanged
                                   ? Py_NewRef(self)
                                   : make_small_parts(Py_TYPE(self), &small);
            clear_parts(parts);
            return result;
        }
    }

    PyObject *old[PART_COUNT] = {0};
    if (parts_from_version(self, old) < 0) {
        clear_parts(old);
        clear_parts(parts);
        return NULL;
    }

    int unchanged = 1;
    for (int i = 0; i < PART_COUNT; i++) {
        if (!parts[i]) {
            parts[i] = Py_NewRef(old[i]);
        }
        else if (unchanged) {
            int equal = PyObject_RichCompareBool(parts[i], old[i], Py_EQ);
            if (equal < 0) {
                clear_parts(old);
                clear_parts(parts);
                return NULL;
            }
            unchanged = equal;
        }
    }
    PyObject *result =
        unchanged ? Py_NewRef(self) : make_parts(Py_TYPE(self), parts);
    clear_parts(old);
    clear_parts(parts);
    return result;
}

/* Keep the general key flat and allocate it only when ordering needs it. */
static PyObject *
build_cmpkey(PyTypeObject *type, PyObject *const *parts)
{
    PyObject *release = parts[PART_RELEASE];
    Py_ssize_t length = PyTuple_GET_SIZE(release);
    while (length != 0) {
        PyObject *last = PyTuple_GET_ITEM(release, length - 1);
        int zero =
            PyLong_CheckExact(last)
                ? PyLong_IsZero(last)
                : PyObject_RichCompareBool(
                      last, Py_GetConstantBorrowed(Py_CONSTANT_ZERO), Py_EQ);
        if (zero < 0) {
            return NULL;
        }
        if (!zero) {
            break;
        }
        length--;
    }
    PyObject *trimmed = length == PyTuple_GET_SIZE(release)
                            ? Py_NewRef(release)
                            : PyTuple_GetSlice(release, 0, length);
    if (trimmed == NULL) {
        return NULL;
    }
    PyObject *pre = parts[PART_PRE];
    PyObject *post = parts[PART_POST];
    PyObject *dev = parts[PART_DEV];
    PyObject *suffix = NULL;
    PyObject *local_key = NULL;
    PyObject *result = NULL;
    if (pre == Py_None && post == Py_None && dev == Py_None) {
        VersionState *state = state_from_type(type);
        if (state == NULL) {
            goto done;
        }
        suffix = Py_NewRef(state->stable_suffix);
    }
    else {
        PyObject *zero = Py_GetConstantBorrowed(Py_CONSTANT_ZERO);
        int rank;
        PyObject *pre_number;
        if (pre == Py_None) {
            rank = post == Py_None && dev != Py_None ? -1 : 3;
            pre_number = zero;
        }
        else {
            PyObject *letter = PyTuple_GET_ITEM(pre, 0);
            rank = PyUnicode_CompareWithASCIIString(letter, "a") == 0   ? 0
                   : PyUnicode_CompareWithASCIIString(letter, "b") == 0 ? 1
                                                                        : 2;
            pre_number = PyTuple_GET_ITEM(pre, 1);
        }
        suffix = Py_BuildValue("(iOiOiO)", rank, pre_number, post != Py_None,
                               post == Py_None ? zero : post, dev == Py_None,
                               dev == Py_None ? zero : dev);
    }
    if (suffix == NULL) {
        goto done;
    }
    PyObject *local = parts[PART_LOCAL];
    if (local == Py_None) {
        result = PyTuple_Pack(3, parts[PART_EPOCH], trimmed, suffix);
        goto done;
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, PyTuple_GET_SIZE(local)) < 0) {
        goto done;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(local); i++) {
        PyObject *part = PyTuple_GET_ITEM(local, i);
        PyObject *key = PyLong_Check(part) ? Py_BuildValue("(Os)", part, "")
                                           : Py_BuildValue("(iO)", -1, part);
        if (key == NULL) {
            tuple_builder_clear(&builder);
            goto done;
        }
        builder.items[i] = key;
    }
    local_key = tuple_builder_finish(&builder);
    if (local_key == NULL) {
        goto done;
    }
    result = PyTuple_Pack(4, parts[PART_EPOCH], trimmed, suffix, local_key);

done:
    Py_DECREF(trimmed);
    Py_XDECREF(suffix);
    Py_XDECREF(local_key);
    return result;
}

static PyObject *
get_cmpkey(PyObject *self)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        PyObject *parts[PART_COUNT] = {0};
        if (parts_from_version(self, parts) < 0) {
            clear_parts(parts);
            return NULL;
        }
        PyObject *key = build_cmpkey(Py_TYPE(self), parts);
        clear_parts(parts);
        return key;
    }

    PyObject *key;
    Py_BEGIN_CRITICAL_SECTION(self);
    key = Py_XNewRef(version->value.full->key);
    Py_END_CRITICAL_SECTION();
    if (key != NULL) {
        return key;
    }
    if (Py_EnterRecursiveCall(" while building a version comparison key")) {
        return NULL;
    }
    PyObject *created =
        build_cmpkey(Py_TYPE(self), version->value.full->parts);
    Py_LeaveRecursiveCall();
    if (created == NULL) {
        return NULL;
    }
    /* Building may invoke an int subclass; publish only after it completes. */
    Py_BEGIN_CRITICAL_SECTION(self);
    if (version->value.full->key == NULL) {
        version->value.full->key = Py_NewRef(created);
    }
    key = Py_NewRef(version->value.full->key);
    Py_END_CRITICAL_SECTION();
    Py_DECREF(created);
    return key;
}

static PyObject *
cmp_to_bool(int comparison, int op)
{
    switch (op) {
    case Py_LT:
        return PyBool_FromLong(comparison < 0);
    case Py_LE:
        return PyBool_FromLong(comparison <= 0);
    case Py_EQ:
        return PyBool_FromLong(comparison == 0);
    case Py_NE:
        return PyBool_FromLong(comparison != 0);
    case Py_GT:
        return PyBool_FromLong(comparison > 0);
    case Py_GE:
        return PyBool_FromLong(comparison >= 0);
    default:
        Py_RETURN_NOTIMPLEMENTED;
    }
}

static PyObject *
version_richcompare(PyObject *self, PyObject *other, int op)
{
    if (Py_TYPE(self) != Py_TYPE(other)) {
        VersionState *state = state_from_type(Py_TYPE(self));
        if (state == NULL) {
            return NULL;
        }
        if (!PyObject_TypeCheck(other, (PyTypeObject *)state->version_type)) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    VersionObject *left = version_cast(self);
    VersionObject *right = version_cast(other);
    if (is_small(left) && is_small(right)) {
        VersionKey a = left->value.key;
        VersionKey b = right->value.key;
        int comparison = a.high < b.high   ? -1
                         : a.high > b.high ? 1
                         : a.low < b.low   ? -1
                         : a.low > b.low   ? 1
                                           : 0;
        return cmp_to_bool(comparison, op);
    }
    PyObject *a = get_cmpkey(self);
    if (a == NULL) {
        return NULL;
    }
    PyObject *b = get_cmpkey(other);
    if (b == NULL) {
        Py_DECREF(a);
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(a, b, op);
    Py_DECREF(a);
    Py_DECREF(b);
    return result;
}

static Py_hash_t
version_hash(PyObject *self)
{
    VersionObject *version = version_cast(self);
    Py_hash_t hash = _Py_atomic_load_ssize_relaxed(&version->hash);
    if (hash != -1) {
        return hash;
    }
    PyObject *key = get_cmpkey(self);
    if (key == NULL) {
        return -1;
    }
    hash = PyObject_Hash(key);
    Py_DECREF(key);
    if (hash == -1) {
        return -1;
    }
    Py_hash_t expected = -1;
    _Py_atomic_compare_exchange_ssize(&version->hash, &expected, hash);
    return expected == -1 ? hash : expected;
}

PyDoc_STRVAR(
    from_parts_doc,
    "from_parts($type, /, *, epoch=0, release, pre=None, post=None, dev=None, "
    "local=None)\n"
    "--\n\n"
    "Build a version directly from validated components.");

PyDoc_STRVAR(
    replace_doc,
    "__replace__($self, /, *, epoch=..., release=..., pre=..., post=..., "
    "dev=..., local=...)\n"
    "--\n\n"
    "Replace components; omitted components retain their values.");

PyDoc_STRVAR(
    sizeof_doc,
    "__sizeof__($self, /)\n"
    "--\n\n"
    "Include the separate full-representation allocation in the object size.");

PyDoc_STRVAR(reduce_doc,
             "__reduce__($self, /)\n"
             "--\n\n"
             "Return constructor arguments and subclass state for pickling.");

static PyMethodDef version_methods[] = {
    {"from_parts", (PyCFunction)(void (*)(void))version_from_parts,
     METH_FASTCALL | METH_KEYWORDS | METH_CLASS, from_parts_doc},
    {"__replace__", (PyCFunction)(void (*)(void))version_replace,
     METH_FASTCALL | METH_KEYWORDS, replace_doc},
    {"__sizeof__", version_sizeof, METH_NOARGS, sizeof_doc},
    {"__reduce__", version_reduce, METH_NOARGS, reduce_doc},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef version_getset[] = {
    {"epoch", get_epoch, NULL, NULL, NULL},
    {"release", get_release, NULL, NULL, NULL},
    {"pre", get_pre, NULL, NULL, NULL},
    {"post", get_post, NULL, NULL, NULL},
    {"dev", get_dev, NULL, NULL, NULL},
    {"local", get_local, NULL, NULL, NULL},
    {"is_prerelease", get_is_prerelease, NULL, NULL, NULL},
    {"is_postrelease", get_is_postrelease, NULL, NULL, NULL},
    {"is_devrelease", get_is_devrelease, NULL, NULL, NULL},
    {"major", get_major, NULL, NULL, NULL},
    {"minor", get_minor, NULL, NULL, NULL},
    {"micro", get_micro, NULL, NULL, NULL},
    {"base_version", get_base_version, NULL, NULL, NULL},
    {"public", get_public, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static int
version_traverse(PyObject *self, visitproc visit, void *arg)
{
    VersionObject *version = version_cast(self);
    Py_VISIT(Py_TYPE(self));
    if (!is_small(version) && version->value.full != NULL) {
        for (int i = 0; i < PART_COUNT; i++) {
            Py_VISIT(version->value.full->parts[i]);
        }
        Py_VISIT(version->value.full->key);
    }
    return 0;
}

static int
version_clear(PyObject *self)
{
    VersionObject *version = version_cast(self);
    if (!is_small(version) && version->value.full != NULL) {
        VersionFull *full = version->value.full;
        version->value.full = NULL;
        clear_parts(full->parts);
        Py_XDECREF(full->key);
        PyMem_Free(full);
    }
    return 0;
}

static void
version_dealloc(PyObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    version_clear(self);
    type->tp_free(self);
    Py_DECREF(type);
}

PyDoc_STRVAR(version_doc, "Version(version)\n"
                          "--\n\n"
                          "An immutable PEP 440 version value.");

static PyType_Slot version_slots[] = {
    {Py_tp_doc, (void *)version_doc},
    {Py_tp_new, version_new},
    {Py_tp_dealloc, version_dealloc},
    {Py_tp_traverse, version_traverse},
    {Py_tp_richcompare, version_richcompare},
    {Py_tp_hash, version_hash},
    {Py_tp_str, version_str},
    {Py_tp_repr, version_repr},
    {Py_tp_getset, version_getset},
    {Py_tp_methods, version_methods},
    {0, NULL},
};

static PyType_Spec version_spec = {
    .name = "_version.Version",
    .basicsize = sizeof(VersionObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
             Py_TPFLAGS_IMMUTABLETYPE,
    .slots = version_slots,
};

static int
version_exec(PyObject *module)
{
    VersionState *state = PyModule_GetState(module);
    state->invalid_version =
        PyErr_NewException("_version.InvalidVersion", PyExc_ValueError, NULL);
    if (state->invalid_version == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(module, "InvalidVersion",
                              state->invalid_version) < 0) {
        return -1;
    }
    state->stable_suffix = Py_BuildValue("(iiiiii)", 3, 0, 0, 0, 1, 0);
    if (state->stable_suffix == NULL) {
        return -1;
    }
    PyObject *module_name = PyModule_GetNameObject(module);
    if (module_name == NULL) {
        return -1;
    }
    PyObject *qualified_name = PyUnicode_FromFormat("%U.Version", module_name);
    Py_DECREF(module_name);
    if (qualified_name == NULL) {
        return -1;
    }
    PyType_Spec spec = version_spec;
    spec.name = PyUnicode_AsUTF8(qualified_name);
    if (spec.name == NULL) {
        Py_DECREF(qualified_name);
        return -1;
    }
    state->version_type = PyType_FromModuleAndSpec(module, &spec, NULL);
    Py_DECREF(qualified_name);
    if (state->version_type == NULL) {
        return -1;
    }
    return PyModule_AddType(module, (PyTypeObject *)state->version_type);
}

static PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, version_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static int
module_traverse(PyObject *module, visitproc visit, void *arg)
{
    VersionState *state = PyModule_GetState(module);
    Py_VISIT(state->version_type);
    Py_VISIT(state->invalid_version);
    Py_VISIT(state->stable_suffix);
    return 0;
}

static int
module_clear(PyObject *module)
{
    VersionState *state = PyModule_GetState(module);
    Py_CLEAR(state->version_type);
    Py_CLEAR(state->invalid_version);
    Py_CLEAR(state->stable_suffix);
    return 0;
}

static void
module_free(void *module)
{
    module_clear((PyObject *)module);
}

static struct PyModuleDef version_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_version",
    .m_doc = "Native PEP 440 version values.",
    .m_size = sizeof(VersionState),
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
    .m_free = module_free,
};

PyMODINIT_FUNC
PyInit__version(void)
{
    return PyModuleDef_Init(&version_module);
}
