#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define READ_MAX_BYTES 65536
const char ah_read_schema[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":4096},\"offset_line\":{\"type\":\"integer\",\"minimum\":1,"
    "\"maximum\":2147483647},\"offset_bytes\":{\"type\":\"integer\",\"minimum\":0,"
    "\"maximum\":9007199254740991},\"max_lines\":{\"type\":\"integer\",\"minimum\":1,"
    "\"maximum\":1000}},\"required\":[\"path\"],\"additionalProperties\":false}";

typedef struct {
    char *content;
    size_t length, lines;
    uint64_t start, next, offset, file_bytes;
    bool truncated, partial_line;
} read_page;

static yyjson_mut_doc *page_receipt(const char *path, const read_page *page) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", true);
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    yyjson_mut_obj_add_strncpy(doc, root, "content", page->content, page->length);
    yyjson_mut_obj_add_uint(doc, root, "bytes", page->length);
    yyjson_mut_obj_add_uint(doc, root, "file_bytes", page->file_bytes);
    if (page->start)
        yyjson_mut_obj_add_uint(doc, root, "start_line", page->start);
    else
        yyjson_mut_obj_add_null(doc, root, "start_line");
    yyjson_mut_obj_add_uint(doc, root, "lines", page->lines);
    if (page->next)
        yyjson_mut_obj_add_uint(doc, root, "next_line", page->next);
    else
        yyjson_mut_obj_add_null(doc, root, "next_line");
    yyjson_mut_obj_add_uint(doc, root, "offset_bytes", page->offset);
    if (page->truncated)
        yyjson_mut_obj_add_uint(doc, root, "next_offset_bytes", page->offset + page->length);
    else
        yyjson_mut_obj_add_null(doc, root, "next_offset_bytes");
    yyjson_mut_obj_add_bool(doc, root, "truncated", page->truncated);
    yyjson_mut_obj_add_bool(doc, root, "partial_line", page->partial_line);
    return doc;
}

static size_t encoded_byte_size(unsigned char c) {
    if (c == '"' || c == '\\' || c == '\b' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
        return 2;
    return c < 0x20 ? 6 : 1;
}

static size_t line_prefix(const char *text, size_t length, size_t limit, size_t *lines) {
    size_t offset = 0;
    *lines = 0;
    while (offset < length) {
        const char *newline = memchr(text + offset, '\n', length - offset);
        if (!newline)
            break;
        offset = (size_t)(newline - text) + 1;
        if (++*lines == limit)
            return offset;
    }
    return length;
}

/* Most pages fit even with worst-case escaping. Scan escapes only near the output bound. */
static size_t output_prefix(const read_page *page, const char *text, size_t length, size_t budget,
                            size_t *accounted, size_t *encoded) {
    if (page->length - *accounted + length <= (budget - *encoded) / 6)
        return length;
    while (*accounted < page->length)
        *encoded += encoded_byte_size((unsigned char)page->content[(*accounted)++]);
    if (length <= (budget - *encoded) / 6)
        return length;
    size_t fits = 0;
    while (fits < length) {
        size_t cost = encoded_byte_size((unsigned char)text[fits]);
        if (cost > budget - *encoded)
            break;
        *encoded += cost;
        ++fits;
    }
    *accounted += fits;
    return fits;
}

static ah_code read_page_from_fd(int fd, const ah_tool_call *call, read_page *page,
                                 size_t max_lines, size_t budget) {
    char buffer[8192];
    uint64_t line = 1;
    size_t encoded = 0, accounted = 0, newlines = 0;
    bool stopped = false;
    ah_code code = AH_OK;
    while (!stopped) {
        code = ah_local_stopped(call);
        if (code != AH_OK)
            break;
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            code = AH_IO;
            break;
        }
        if (!count)
            break;
        size_t offset = 0, length = (size_t)count;
        while (offset < length && line < page->start) {
            const char *newline = memchr(buffer + offset, '\n', length - offset);
            size_t skip = newline ? (size_t)(newline - buffer) + 1 - offset : length - offset;
            page->offset += skip;
            offset += skip;
            if (newline)
                ++line;
        }
        if (offset == length)
            continue;
        if (newlines == max_lines || page->length == READ_MAX_BYTES) {
            page->truncated = true;
            break;
        }
        size_t available = length - offset, capacity = READ_MAX_BYTES - page->length;
        size_t selected_lines;
        size_t selected = line_prefix(buffer + offset, available < capacity ? available : capacity,
                                      max_lines - newlines, &selected_lines);
        size_t fits = output_prefix(page, buffer + offset, selected, budget, &accounted, &encoded);
        if (fits < selected)
            line_prefix(buffer + offset, fits, max_lines - newlines, &selected_lines);
        memcpy(page->content + page->length, buffer + offset, fits);
        page->length += fits;
        newlines += selected_lines;
        if (fits < available)
            page->truncated = stopped = true;
    }
    if (code != AH_OK)
        return code;
    size_t complete;
    if (memchr(page->content, '\0', page->length) ||
        !ah_local_utf8(page->content, page->length, &complete) ||
        (!page->truncated && complete != page->length))
        return AH_INVALID;
    page->length = complete;
    if (page->truncated && !page->length)
        return AH_LIMIT;
    bool tail = page->length && page->content[page->length - 1] != '\n';
    page->lines = newlines + (tail ? 1 : 0);
    page->partial_line = page->truncated && tail;
    page->next = page->truncated && page->start ? page->start + newlines : 0;
    return AH_OK;
}

ah_code ah_local_read_file(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    ah_error local_error;
    if (!error)
        error = &local_error;
    yyjson_doc *args = ah_local_arguments(call, ah_read_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *path = yyjson_obj_get(root, "path");
    yyjson_val *offset = yyjson_obj_get(root, "offset_line");
    yyjson_val *byte_offset = yyjson_obj_get(root, "offset_bytes");
    yyjson_val *requested = yyjson_obj_get(root, "max_lines");
    if (offset && byte_offset) {
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_INVALID,
                              "Use either offset_line or offset_bytes, not both");
    }
    read_page page = {.content = "",
                      .start = byte_offset ? 0
                               : offset    ? (uint64_t)yyjson_get_num(offset)
                                           : 1,
                      .offset = byte_offset ? (uint64_t)yyjson_get_num(byte_offset) : 0};
    size_t max_lines = requested ? (size_t)yyjson_get_num(requested) : 200;
    char leaf[4097];
    int parent = ah_local_parent_fd(context, path, leaf);
    if (parent < 0) {
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_INVALID,
                              "Path is invalid or cannot be traversed without symlinks");
    }
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(parent);
    struct stat st;
    ah_code code = fd < 0 ? AH_IO : AH_OK;
    if (code == AH_OK && (fstat(fd, &st) || !S_ISREG(st.st_mode)))
        code = AH_INVALID;
    if (code != AH_OK) {
        if (fd >= 0)
            close(fd);
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, code,
                              "Cannot read a regular file without following symlinks");
    }
    page.file_bytes = (uint64_t)st.st_size;
    if (byte_offset && lseek(fd, (off_t)page.offset, SEEK_SET) < 0) {
        close(fd);
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_IO, "Cannot seek to byte offset");
    }
    yyjson_mut_doc *sizing = page_receipt(yyjson_get_str(path), &page);
    char *empty = ah_mut_write(sizing);
    ah_mut_doc_free(sizing);
    /* Allow for larger byte, line, and continuation numbers in the final receipt. */
    size_t overhead = empty ? strlen(empty) + 64 : 0;
    free(empty);
    size_t limit = ah_local_limit(call), budget = limit > overhead ? limit - overhead : 0;
    page.content = overhead && budget ? malloc(READ_MAX_BYTES) : NULL;
    if (!overhead || !page.content) {
        close(fd);
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, budget ? AH_IO : AH_LIMIT,
                              "Cannot allocate read buffer or output limit is too small");
    }
    code = read_page_from_fd(fd, call, &page, max_lines, budget);
    close(fd);
    yyjson_mut_doc *doc = code == AH_OK ? page_receipt(yyjson_get_str(path), &page) : NULL;
    free(page.content);
    yyjson_doc_free(args);
    if (code != AH_OK)
        return ah_local_error(call, result, error, code,
                              "Read failed, was interrupted, or selected content is not UTF-8");
    return ah_local_finish(call, result, doc, error);
}
