#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <string.h>
#include <stdlib.h>

#include "filter.h"

struct FilterEntry {
    char *pattern;  /* stored lower-cased if both glob and nocase */
    bool glob;
    bool nocase;
    FFType types;   /* FFT_ANY if not restricted */
};

struct FileFilter {
    struct FilterEntry *entries;
    size_t count;
    size_t capacity;
};

/* Only ASCII letters are folded, independently of the locale. */
static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ascii_equal_nocase(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static char *ascii_lower_dup(const char *s)
{
    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (result == NULL)
        return NULL;
    for (size_t i = 0; i <= len; ++i)
        result[i] = ascii_lower(s[i]);
    return result;
}

/* True if `s` consists of exactly `len` lower-case hex digits. */
static bool is_lower_hex(const char *s, size_t len)
{
    if (strlen(s) != len)
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}

/*
 * Names under which a file that is unlinked while still open is kept until
 * it is released. They are created and removed through the mount, so they
 * must never be filtered:
 *
 *   libfuse:  ".fuse_hidden%08x%08x"
 *   fuse-t:   ".nfs.%08x.%04x" (the "silly rename" of the macOS NFS client
 *             that fuse-t is built on)
 */
static bool is_temporary_name(const char *name)
{
    static const char fuse_prefix[] = ".fuse_hidden";
    if (strncmp(name, fuse_prefix, sizeof(fuse_prefix) - 1) == 0)
        return is_lower_hex(name + sizeof(fuse_prefix) - 1, 16);

#ifdef HAVE_FUSE_T
    static const char nfs_prefix[] = ".nfs.";
    if (strncmp(name, nfs_prefix, sizeof(nfs_prefix) - 1) == 0) {
        const char *p = name + sizeof(nfs_prefix) - 1;
        char first[9];
        if (strlen(p) != 8 + 1 + 4 || p[8] != '.')
            return false;
        memcpy(first, p, 8);
        first[8] = '\0';
        return is_lower_hex(first, 8) && is_lower_hex(p + 9, 4);
    }
#endif

    return false;
}

static bool entry_matches_name(const struct FilterEntry *e, const char *name)
{
    if (!e->glob) {
        if (e->nocase)
            return ascii_equal_nocase(e->pattern, name);
        return strcmp(e->pattern, name) == 0;
    }

    if (!e->nocase)
        return fnmatch(e->pattern, name, 0) == 0;

    char *lowered = ascii_lower_dup(name);
    if (lowered == NULL)
        return true; /* out of memory: err on the side of hiding */
    bool result = fnmatch(e->pattern, lowered, 0) == 0;
    free(lowered);
    return result;
}

/* Takes ownership of `pattern`, also on failure. */
static bool add_entry(FileFilter *f, char *pattern, bool glob, bool nocase, FFType types)
{
    for (size_t i = 0; i < f->count; ++i) {
        const struct FilterEntry *e = &f->entries[i];
        if (e->glob == glob && e->nocase == nocase && e->types == types
                && strcmp(e->pattern, pattern) == 0) {
            free(pattern);
            return true;
        }
    }

    if (f->count == f->capacity) {
        size_t new_capacity = f->capacity == 0 ? 4 : f->capacity * 2;
        struct FilterEntry *new_entries =
            realloc(f->entries, new_capacity * sizeof(struct FilterEntry));
        if (new_entries == NULL) {
            free(pattern);
            return false;
        }
        f->entries = new_entries;
        f->capacity = new_capacity;
    }

    struct FilterEntry *e = &f->entries[f->count++];
    e->pattern = pattern;
    e->glob = glob;
    e->nocase = nocase;
    e->types = types;
    return true;
}

FileFilter *filefilter_create(void)
{
    FileFilter *f = malloc(sizeof(FileFilter));
    if (f == NULL)
        return NULL;
    f->entries = NULL;
    f->count = 0;
    f->capacity = 0;
    return f;
}

void filefilter_destroy(FileFilter *f)
{
    if (f == NULL)
        return;
    for (size_t i = 0; i < f->count; ++i)
        free(f->entries[i].pattern);
    free(f->entries);
    free(f);
}

bool filefilter_is_empty(const FileFilter *f)
{
    return f == NULL || f->count == 0;
}

bool filefilter_has_typed(const FileFilter *f)
{
    if (f == NULL)
        return false;
    for (size_t i = 0; i < f->count; ++i) {
        if (f->entries[i].types != FFT_ANY)
            return true;
    }
    return false;
}

static bool starts_with(const char *s, const char *prefix, size_t *prefix_len)
{
    *prefix_len = strlen(prefix);
    return strncmp(s, prefix, *prefix_len) == 0;
}

static FFType parse_type_word(const char *word, size_t len)
{
    static const struct { const char *word; FFType type; } types[] = {
        { "file", FFT_REG },
        { "dir", FFT_DIR },
        { "symlink", FFT_LNK },
        { "socket", FFT_SCK },
        { "fifo", FFT_PIP },
        { "block", FFT_BLK },
        { "char", FFT_CHR },
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (strlen(types[i].word) == len && strncmp(types[i].word, word, len) == 0)
            return types[i].type;
    }
    return FFT_UNKNOWN;
}

static bool ends_with_unescaped_backslash(const char *pattern)
{
    size_t len = strlen(pattern);
    size_t backslashes = 0;
    while (backslashes < len && pattern[len - 1 - backslashes] == '\\')
        ++backslashes;
    return backslashes % 2 == 1;
}

bool filefilter_add_spec(FileFilter *f, const char *spec, const char **err)
{
    static const struct { const char *key; bool glob; bool nocase; } name_keys[] = {
        { "name=", false, false },
        { "name-glob=", true, false },
        { "iname=", false, true },
        { "iname-glob=", true, true },
    };
    const char *dummy_err;
    FFType types = 0;
    const char *p = spec;
    size_t key_len;

    if (err == NULL)
        err = &dummy_err;

    while (starts_with(p, "type=", &key_len)) {
        p += key_len;
        const char *end = strchr(p, ':');
        if (end == NULL) {
            *err = "missing name=, name-glob=, iname= or iname-glob= after type=";
            return false;
        }
        FFType type = parse_type_word(p, (size_t)(end - p));
        if (type == FFT_UNKNOWN) {
            *err = "unknown file type (expected file, dir, symlink, socket, fifo, block or char)";
            return false;
        }
        types |= type;
        p = end + 1;
    }

    for (size_t i = 0; i < sizeof(name_keys) / sizeof(name_keys[0]); ++i) {
        if (!starts_with(p, name_keys[i].key, &key_len))
            continue;

        const char *name = p + key_len;
        bool glob = name_keys[i].glob;
        bool nocase = name_keys[i].nocase;

        if (*name == '\0') {
            *err = "empty name";
            return false;
        }
        if (strchr(name, '/') != NULL) {
            *err = "name contains '/' (matching by path is not supported)";
            return false;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            *err = "'.' and '..' cannot be filtered";
            return false;
        }
        if (glob && ends_with_unescaped_backslash(name)) {
            *err = "pattern ends with an unescaped backslash";
            return false;
        }

        char *pattern = (glob && nocase) ? ascii_lower_dup(name) : strdup(name);
        if (pattern == NULL
                || !add_entry(f, pattern, glob, nocase, types == 0 ? FFT_ANY : types)) {
            *err = "out of memory";
            return false;
        }
        return true;
    }

    *err = "expected [type=TYPE:]... followed by name=, name-glob=, iname= or iname-glob=";
    return false;
}

FFResult filefilter_match(const FileFilter *f, const char *name, FFType type)
{
    bool needs_type = false;

    if (filefilter_is_empty(f) || is_temporary_name(name))
        return FF_NO_MATCH;

    for (size_t i = 0; i < f->count; ++i) {
        const struct FilterEntry *e = &f->entries[i];

        if (e->types != FFT_ANY) {
            /* Skip the name comparison if the type rules this entry out. */
            if (type != FFT_UNKNOWN && !(e->types & type))
                continue;
            if (type == FFT_UNKNOWN && needs_type)
                continue;
        }

        if (!entry_matches_name(e, name))
            continue;

        if (e->types == FFT_ANY || type != FFT_UNKNOWN)
            return FF_MATCH;
        needs_type = true;
    }

    return needs_type ? FF_NEEDS_TYPE : FF_NO_MATCH;
}

/* Like filefilter_match, but never returns FF_NEEDS_TYPE: a name that
 * matches a type-restricted filter and whose type cannot be told is
 * treated as matching. */
static bool matches_with_known_type(const FileFilter *f, const char *name, FFType type)
{
    return filefilter_match(f, name, type) != FF_NO_MATCH;
}

int filefilter_check_path(const FileFilter *f, const char *path,
                          enum FilterIntent intent, FFType new_type,
                          filefilter_stat_fn stat_fn, void *ctx)
{
    int result = 0;

    if (filefilter_is_empty(f))
        return 0;

    while (*path == '/')
        ++path;

    /* A private copy, so that it can be cut off after each component. */
    char *buf = strdup(path);
    if (buf == NULL)
        return ENOMEM;

    char *name = buf;
    while (*name != '\0') {
        char *end = name + strcspn(name, "/");
        char *next = end + strspn(end, "/");
        bool is_final = (*next == '\0');
        bool creating = is_final && intent == FILTER_CREATE;
        char saved = *end;

        *end = '\0';  /* now `buf` is the path up to this component */

        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            bool hidden;

            switch (filefilter_match(f, name, FFT_UNKNOWN)) {
            case FF_MATCH:
                hidden = true;
                break;
            case FF_NEEDS_TYPE: {
                FFType type = FFT_UNKNOWN;
                int err = (stat_fn != NULL) ? stat_fn(ctx, buf, &type) : 0;

                if (err == 0) {
                    /* The existing object decides first... */
                    hidden = matches_with_known_type(f, name, type);
                    /* ...but replacing it must not produce a hidden one. */
                    if (!hidden && creating && new_type != FFT_UNKNOWN)
                        hidden = matches_with_known_type(f, name, new_type);
                } else if (err == ENOENT && creating) {
                    hidden = matches_with_known_type(f, name, new_type);
                } else {
                    result = err;
                    goto out;
                }
                break;
            }
            default:
                hidden = false;
                break;
            }

            if (hidden) {
                result = creating ? EPERM : ENOENT;
                goto out;
            }
        }

        *end = saved;
        name = next;
    }

out:
    free(buf);
    return result;
}

bool filefilter_hides_entry(const FileFilter *f, const char *name, FFType type,
                            filefilter_stat_fn stat_fn, void *ctx)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;

    switch (filefilter_match(f, name, type)) {
    case FF_MATCH:
        return true;
    case FF_NEEDS_TYPE:
        if (stat_fn == NULL || stat_fn(ctx, name, &type) != 0)
            return true;
        return matches_with_known_type(f, name, type);
    default:
        return false;
    }
}

FFType filefilter_type_from_mode(mode_t mode)
{
    if (S_ISREG(mode))
        return FFT_REG;
    if (S_ISDIR(mode))
        return FFT_DIR;
    if (S_ISLNK(mode))
        return FFT_LNK;
    if (S_ISSOCK(mode))
        return FFT_SCK;
    if (S_ISFIFO(mode))
        return FFT_PIP;
    if (S_ISBLK(mode))
        return FFT_BLK;
    if (S_ISCHR(mode))
        return FFT_CHR;
    return FFT_UNKNOWN;
}

FFType filefilter_type_from_dtype(unsigned char d_type)
{
#ifdef DT_UNKNOWN
    switch (d_type) {
    case DT_REG:
        return FFT_REG;
    case DT_DIR:
        return FFT_DIR;
    case DT_LNK:
        return FFT_LNK;
    case DT_SOCK:
        return FFT_SCK;
    case DT_FIFO:
        return FFT_PIP;
    case DT_BLK:
        return FFT_BLK;
    case DT_CHR:
        return FFT_CHR;
    default:
        return FFT_UNKNOWN;
    }
#else
    (void)d_type;
    return FFT_UNKNOWN;
#endif
}

