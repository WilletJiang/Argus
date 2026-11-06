#include "ah/tools.h"
#include "../internal.h"
#include <math.h>

#define SCHEMA_DEPTH 32
#define JSON_NODE_LIMIT 100000
#define JSON_TEXT_LIMIT (4 * 1024 * 1024)

static bool same_key(yyjson_val *a, yyjson_val *b) {
    return yyjson_get_len(a) == yyjson_get_len(b) &&
           memcmp(yyjson_get_str(a), yyjson_get_str(b), yyjson_get_len(a)) == 0;
}

/* yyjson accepts duplicate members; reject them before either schema or data use. */
static bool unambiguous(yyjson_val *v, unsigned depth, size_t *remaining) {
    if (depth > SCHEMA_DEPTH || !*remaining || yyjson_is_raw(v))
        return false;
    --*remaining;
    if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 1024)
            return false;
        yyjson_obj_iter outer = yyjson_obj_iter_with(v);
        yyjson_val *key;
        size_t index = 0;
        while ((key = yyjson_obj_iter_next(&outer))) {
            yyjson_obj_iter inner = yyjson_obj_iter_with(v);
            for (size_t j = 0; j < index; ++j)
                if (same_key(key, yyjson_obj_iter_next(&inner)))
                    return false;
            ++index;
            if (!unambiguous(yyjson_obj_iter_get_val(key), depth + 1, remaining))
                return false;
        }
    } else if (yyjson_is_arr(v)) {
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(v, i, n,
                           item) if (!unambiguous(item, depth + 1, remaining)) return false;
    }
    return true;
}

static bool named(yyjson_val *v, const char *name) {
    return yyjson_equals_str(v, name);
}
static bool supported_type(yyjson_val *v) {
    return named(v, "object") || named(v, "array") || named(v, "string") || named(v, "integer") ||
           named(v, "number") || named(v, "boolean") || named(v, "null");
}

static long double number(yyjson_val *v) {
    if (yyjson_is_uint(v))
        return (long double)yyjson_get_uint(v);
    if (yyjson_is_sint(v))
        return (long double)yyjson_get_sint(v);
    return (long double)yyjson_get_real(v);
}

static uint64_t magnitude(yyjson_val *v) {
    if (yyjson_is_uint(v))
        return yyjson_get_uint(v);
    int64_t n = yyjson_get_sint(v);
    return n < 0 ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
}

/* Keep uint64 bounds exact even where long double has only double precision. */
static int compare_numbers(yyjson_val *a, yyjson_val *b) {
    bool aneg = number(a) < 0, bneg = number(b) < 0;
    if (aneg != bneg)
        return aneg ? -1 : 1;
    int order;
    if (yyjson_is_int(a) && yyjson_is_int(b)) {
        uint64_t x = magnitude(a), y = magnitude(b);
        order = x < y ? -1 : x > y;
    } else if (yyjson_is_real(a) && yyjson_is_real(b)) {
        double x = fabs(yyjson_get_real(a)), y = fabs(yyjson_get_real(b));
        order = x < y ? -1 : x > y;
    } else {
        bool aint = yyjson_is_int(a);
        uint64_t integer = magnitude(aint ? a : b);
        double real = fabs(yyjson_get_real(aint ? b : a));
        if (real >= 18446744073709551616.0)
            order = -1;
        else {
            uint64_t whole = (uint64_t)real;
            order = integer < whole ? -1 : integer > whole ? 1 : real > floor(real) ? -1 : 0;
        }
        if (!aint)
            order = -order;
    }
    return aneg ? -order : order;
}

static bool equal_values(yyjson_val *a, yyjson_val *b) {
    if (yyjson_is_num(a) && yyjson_is_num(b))
        return compare_numbers(a, b) == 0;
    if (yyjson_is_arr(a) && yyjson_is_arr(b)) {
        if (yyjson_arr_size(a) != yyjson_arr_size(b))
            return false;
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(a, i, n,
                           item) if (!equal_values(item, yyjson_arr_get(b, i))) return false;
        return true;
    }
    if (yyjson_is_obj(a) && yyjson_is_obj(b)) {
        if (yyjson_obj_size(a) != yyjson_obj_size(b))
            return false;
        yyjson_obj_iter it = yyjson_obj_iter_with(a);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val *other = yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key));
            if (!other || !equal_values(yyjson_obj_iter_get_val(key), other))
                return false;
        }
        return true;
    }
    return yyjson_equals(a, b);
}

static bool count_bound(yyjson_val *v) {
    return yyjson_is_uint(v) && yyjson_get_uint(v) <= SIZE_MAX;
}

static bool check_schema(yyjson_val *schema, unsigned depth) {
    if (depth > SCHEMA_DEPTH || !yyjson_is_obj(schema))
        return false;
    yyjson_val *type = yyjson_obj_get(schema, "type");
    if (type && !supported_type(type))
        return false;
    yyjson_obj_iter it = yyjson_obj_iter_with(schema);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it))) {
        yyjson_val *v = yyjson_obj_iter_get_val(key);
        if (named(key, "type"))
            continue;
        if (named(key, "description") || named(key, "title")) {
            if (!yyjson_is_str(v))
                return false;
        } else if (named(key, "properties")) {
            if (!named(type, "object") || !yyjson_is_obj(v))
                return false;
            yyjson_obj_iter p = yyjson_obj_iter_with(v);
            yyjson_val *property;
            while ((property = yyjson_obj_iter_next(&p)))
                if (!check_schema(yyjson_obj_iter_get_val(property), depth + 1))
                    return false;
        } else if (named(key, "required")) {
            if (!named(type, "object") || !yyjson_is_arr(v))
                return false;
            size_t i, n;
            yyjson_val *entry;
            yyjson_arr_foreach(v, i, n, entry) {
                if (!yyjson_is_str(entry))
                    return false;
                for (size_t j = 0; j < i; ++j)
                    if (same_key(entry, yyjson_arr_get(v, j)))
                        return false;
            }
        } else if (named(key, "additionalProperties")) {
            if (!named(type, "object") || !yyjson_is_bool(v))
                return false;
        } else if (named(key, "items")) {
            if (!named(type, "array") || !check_schema(v, depth + 1))
                return false;
        } else if (named(key, "enum")) {
            if (!yyjson_is_arr(v) || !yyjson_arr_size(v))
                return false;
        } else if (named(key, "minLength") || named(key, "maxLength")) {
            if (!named(type, "string") || !count_bound(v))
                return false;
        } else if (named(key, "minItems") || named(key, "maxItems")) {
            if (!named(type, "array") || !count_bound(v))
                return false;
        } else if (named(key, "minimum") || named(key, "maximum")) {
            if ((!named(type, "number") && !named(type, "integer")) || !yyjson_is_num(v))
                return false;
        } else
            return false;
    }
    const char *bounds[][2] = {
        {"minimum", "maximum"}, {"minLength", "maxLength"}, {"minItems", "maxItems"}};
    for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i) {
        yyjson_val *low = yyjson_obj_get(schema, bounds[i][0]);
        yyjson_val *high = yyjson_obj_get(schema, bounds[i][1]);
        if (low && high && compare_numbers(low, high) > 0)
            return false;
    }
    return true;
}

static bool matches_type(yyjson_val *type, yyjson_val *v) {
    if (!type)
        return true;
    if (named(type, "object"))
        return yyjson_is_obj(v);
    if (named(type, "array"))
        return yyjson_is_arr(v);
    if (named(type, "string"))
        return yyjson_is_str(v);
    if (named(type, "boolean"))
        return yyjson_is_bool(v);
    if (named(type, "null"))
        return yyjson_is_null(v);
    if (named(type, "number"))
        return yyjson_is_num(v);
    return yyjson_is_int(v) ||
           (yyjson_is_real(v) && floor(yyjson_get_real(v)) == yyjson_get_real(v));
}

static bool within(yyjson_val *schema, const char *low, const char *high, long double n) {
    yyjson_val *min = yyjson_obj_get(schema, low), *max = yyjson_obj_get(schema, high);
    return (!min || n >= number(min)) && (!max || n <= number(max));
}

static bool validate(yyjson_val *schema, yyjson_val *v, unsigned depth) {
    if (depth > SCHEMA_DEPTH || !matches_type(yyjson_obj_get(schema, "type"), v))
        return false;
    yyjson_val *choices = yyjson_obj_get(schema, "enum");
    if (choices) {
        size_t i, n;
        yyjson_val *choice;
        bool found = false;
        yyjson_arr_foreach(choices, i, n, choice) if (equal_values(choice, v)) found = true;
        if (!found)
            return false;
    }
    if (yyjson_is_obj(v)) {
        yyjson_val *required = yyjson_obj_get(schema, "required");
        size_t i, n;
        yyjson_val *key;
        yyjson_arr_foreach(
            required, i, n,
            key) if (!yyjson_obj_getn(v, yyjson_get_str(key), yyjson_get_len(key))) return false;
        yyjson_val *properties = yyjson_obj_get(schema, "properties");
        yyjson_val *additional = yyjson_obj_get(schema, "additionalProperties");
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val *child =
                yyjson_obj_getn(properties, yyjson_get_str(key), yyjson_get_len(key));
            if (child) {
                if (!validate(child, yyjson_obj_iter_get_val(key), depth + 1))
                    return false;
            } else if (additional && !yyjson_get_bool(additional))
                return false;
        }
    } else if (yyjson_is_arr(v)) {
        if (!within(schema, "minItems", "maxItems", yyjson_arr_size(v)))
            return false;
        yyjson_val *items = yyjson_obj_get(schema, "items");
        if (items) {
            size_t i, n;
            yyjson_val *entry;
            yyjson_arr_foreach(v, i, n, entry) if (!validate(items, entry, depth + 1)) return false;
        }
    } else if (yyjson_is_str(v)) {
        size_t count = 0, length = yyjson_get_len(v);
        const unsigned char *s = (const unsigned char *)yyjson_get_str(v);
        for (size_t i = 0; i < length; ++i)
            if ((s[i] & 0xc0) != 0x80)
                ++count;
        if (!within(schema, "minLength", "maxLength", count))
            return false;
    } else if (yyjson_is_num(v)) {
        yyjson_val *min = yyjson_obj_get(schema, "minimum"),
                   *max = yyjson_obj_get(schema, "maximum");
        if ((min && compare_numbers(v, min) < 0) || (max && compare_numbers(v, max) > 0))
            return false;
    }
    return true;
}

ah_code ah_schema_check(const char *schema_json, ah_error *error) {
    if (!schema_json || strlen(schema_json) > JSON_TEXT_LIMIT)
        return ah_fail(error, AH_INVALID, "Missing or oversized JSON schema");
    yyjson_doc *schema = yyjson_read(schema_json, strlen(schema_json), YYJSON_READ_BIGNUM_AS_RAW);
    size_t budget = JSON_NODE_LIMIT;
    bool valid = schema && unambiguous(yyjson_doc_get_root(schema), 0, &budget) &&
                 check_schema(yyjson_doc_get_root(schema), 0);
    yyjson_doc_free(schema);
    if (!valid)
        return ah_fail(error, AH_INVALID, "Unsupported or invalid JSON schema");
    ah_error_clear(error);
    return AH_OK;
}

ah_code ah_schema_parse(const char *schema_json, const char *value_json, yyjson_doc **parsed,
                        ah_error *error) {
    *parsed = NULL;
    if (!schema_json || !value_json || strlen(schema_json) > JSON_TEXT_LIMIT ||
        strlen(value_json) > JSON_TEXT_LIMIT)
        return ah_fail(error, AH_INVALID, "Missing or oversized JSON schema/value");
    yyjson_doc *schema = yyjson_read(schema_json, strlen(schema_json), YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_doc *value = yyjson_read(value_json, strlen(value_json), YYJSON_READ_BIGNUM_AS_RAW);
    size_t schema_budget = JSON_NODE_LIMIT, value_budget = JSON_NODE_LIMIT;
    bool valid_schema = schema && unambiguous(yyjson_doc_get_root(schema), 0, &schema_budget) &&
                        check_schema(yyjson_doc_get_root(schema), 0);
    bool valid_value = value && unambiguous(yyjson_doc_get_root(value), 0, &value_budget);
    bool accepted = valid_schema && valid_value &&
                    validate(yyjson_doc_get_root(schema), yyjson_doc_get_root(value), 0);
    yyjson_doc_free(schema);
    if (!accepted) {
        yyjson_doc_free(value);
        return ah_fail(error, AH_INVALID, "%s",
                       !valid_schema  ? "Unsupported or invalid JSON schema"
                       : !valid_value ? "Invalid, ambiguous, or excessive JSON value"
                                      : "Tool arguments do not match schema");
    }
    *parsed = value;
    ah_error_clear(error);
    return AH_OK;
}

ah_code ah_schema_validate(const char *schema_json, const char *value_json, ah_error *error) {
    yyjson_doc *parsed;
    ah_code code = ah_schema_parse(schema_json, value_json, &parsed, error);
    yyjson_doc_free(parsed);
    return code;
}
