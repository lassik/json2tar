//! Copyright 2021 Lassi Kortela (json2tar)
//! SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>

#include <jsont.h>

#define MAXDEPTH 64
#define CHUNK 4096

static unsigned char *inbuf;
static size_t incap;
static size_t inlen;

static void panic(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void panic_memory(void) { panic("out of memory"); }

static void slurp(FILE *input)
{
    do {
        if (!incap)
            incap = CHUNK;
        while (incap - inlen < CHUNK)
            incap *= 2;
        if (!(inbuf = realloc(inbuf, incap)))
            panic_memory();
        size_t nread;
        nread = fread(inbuf + inlen, 1, CHUNK, input);
        if (ferror(input))
            panic("read error");
        inlen += nread;
    } while (!feof(input));
    memset(inbuf + inlen, 0, incap - inlen);
}

struct stack_entry {
    char *object_field;
    size_t array_length;
    int type; // 'A' 'O' 0
};

static const char zeros[512];
static char tar[512];
static char *const tar_type = &tar[156];
static char *const path = tar;
static const size_t path_size = 100;
static size_t path_len;
static struct stack_entry stack[MAXDEPTH]; // Zeroth entry is a dummy one.
static struct stack_entry *stack_top = stack;

static int stack_empty() { return stack_top == stack; }

static void stack_push(int type)
{
    if (stack_top >= &stack[MAXDEPTH - 1])
        panic("too deep");
    struct stack_entry *entry = ++stack_top;
    entry->type = type;
}

static void stack_pop(void)
{
    free(stack_top->object_field);
    memset(stack_top, 0, sizeof(*stack_top));
    stack_top--;
}

static void set_object_field(struct stack_entry *entry, char *field)
{
    free(entry->object_field);
    entry->object_field = field;
}

static int safe_path_char(int ch)
{
    char url_unreserved_chars[] = "-._~";
    return (((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z'))
        || ((ch >= '0') && (ch <= '9')) || strchr(url_unreserved_chars, ch));
}

static int path_empty() { return !path_len; }

static void path_clear(void)
{
    memset(path, 0, path_size);
    path_len = 0;
}

static void path_putc(int c)
{
    if (path_len >= path_size - 1) {
        panic("path too long");
    }
    path[path_len++] = c;
}

static void path_puts(const char *s)
{
    for (; *s; s++) {
        path_putc(*s);
    }
}

static void path_puts_url(const char *s)
{
    const char hex[16] = "0123456789ABCDEF";
    for (; *s; s++) {
        if (safe_path_char(*s)) {
            path_putc(*s);
        } else {
            unsigned char byte = *s;
            path_putc('%');
            path_putc(hex[0xf & (byte / 16)]);
            path_putc(hex[0xf & byte]);
        }
    }
}

static void path_putu_size(size_t size)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%zu", size);
    path_puts(buf);
}

static void build_path(void)
{
    path_clear();
    if (stack_empty()) {
        path_puts("root");
        return;
    }
    struct stack_entry *entry;
    for (entry = stack + 1; entry <= stack_top; entry++) {
        if (!path_empty()) {
            path_putc('/');
        }
        if (entry->type == 'O') {
            if (entry->object_field) {
                path_puts_url(entry->object_field);
            }
        } else if (entry->type == 'A') {
            if (entry->array_length) {
                path_putu_size(entry->array_length - 1);
            }
        }
    }
}

static void write_bytes(const void *bytes, size_t nbyte)
{
    if (fwrite(bytes, 1, nbyte, stdout) != nbyte)
        panic("write error");
    if (ferror(stdout))
        panic("write error");
}

static size_t tar_padding(size_t nbyte)
{
    nbyte = 512 - (nbyte % 512);
    if (nbyte == 512)
        nbyte = 0;
    return nbyte;
}

static unsigned long tar_checksum(void)
{
    size_t i;
    uint8_t *unsigned_tar = (uint8_t *)tar;
    uint32_t sum = 0;
    for (i = 0; i < 512; i++)
        sum += unsigned_tar[i];
    return sum % 0777777;
}

static void write_tar_header(void)
{
    snprintf(&tar[257], 6, "ustar");
    tar[263] = '0';
    tar[264] = '0';
    for (size_t i = 0; i < 8; i++)
        tar[148 + i] = ' ';
    snprintf(&tar[148], 8, "%07lou", tar_checksum());
    write_bytes(tar, sizeof(tar));
}

static void tar_regular_file(const void *bytes, size_t nbyte)
{
    *tar_type = '0';
    snprintf(&tar[100], 8, "%7lou", (unsigned long)0644);
    snprintf(&tar[124], 12, "%011zo", nbyte);
    write_tar_header();
    write_bytes(bytes, nbyte);
    write_bytes(zeros, tar_padding(nbyte));
}

static void tar_directory(void)
{
    *tar_type = '5';
    snprintf(&tar[100], 8, "%7lou", (unsigned long)0755);
    write_tar_header();
}

static void before_value(void)
{
    if (!stack_empty()) {
        if (stack_top->type == 'A')
            stack_top->array_length++;
    }
}

static void after_value(void)
{
    if (!stack_empty()) {
        if (stack_top->type == 'O') {
            set_object_field(stack_top, 0);
        }
    }
}

static void json2tar(void)
{
    jsont_ctx_t *jsont = jsont_create(0);
    jsont_reset(jsont, inbuf, inlen);
    int done = 0;
    const char *value_string;
    const uint8_t *value_bytes;
    size_t value_nbyte;
    while (!done) {
        jsont_tok_t tok = jsont_next(jsont);
        switch (tok) {
        case JSONT_END:
            done = 1;
            break;
        case JSONT_ERR:
            panic("JSON read error");
            break;
            break;
        case JSONT_FIELD_NAME: {
            char *field = jsont_strcpy_value(jsont);
            if (!field)
                panic_memory();
            set_object_field(stack_top, field);
            break;
        }
        case JSONT_ARRAY_START:
            before_value();
            if (!stack_empty()) {
                build_path();
                tar_directory();
            }
            stack_push('A');
            after_value();
            break;
        case JSONT_OBJECT_START:
            before_value();
            if (!stack_empty()) {
                build_path();
                tar_directory();
            }
            stack_push('O');
            after_value();
            break;
        case JSONT_OBJECT_END: //! Fallthrough
        case JSONT_ARRAY_END:
            stack_pop();
            break;
        case JSONT_TRUE: //! Fallthrough
        case JSONT_FALSE: //! Fallthrough
        case JSONT_NULL: //! Fallthrough
        case JSONT_NUMBER_INT: //! Fallthrough
        case JSONT_NUMBER_FLOAT: //! Fallthrough
        case JSONT_STRING: {
            before_value();
            build_path();
            if (tok == JSONT_TRUE) {
                value_string = "true";
            } else if (tok == JSONT_FALSE) {
                value_string = "false";
            } else if (tok == JSONT_NULL) {
                value_string = "null";
            } else {
                value_string = 0;
            }
            if (value_string) {
                value_bytes = (const uint8_t *)value_string;
                value_nbyte = strlen(value_string);
            } else {
                value_nbyte = jsont_data_value(jsont, &value_bytes);
            }
            if (value_nbyte && !value_bytes)
                panic_memory();
            tar_regular_file(value_bytes, value_nbyte);
            after_value();
            break;
        }
        default:
            break;
        }
    }
    jsont_destroy(jsont);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    slurp(stdin);
    json2tar();
    return 0;
}
