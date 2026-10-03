#ifndef INC_BINDFS_FILTER_H
#define INC_BINDFS_FILTER_H

#include <config.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * File filter: a set of rules that hide directory entries by name and,
 * optionally, by file type.
 *
 * This module has no FUSE dependency. Everything that touches the
 * filesystem goes through a caller-supplied callback, so the decision
 * logic can be unit tested.
 */

struct FileFilter;
typedef struct FileFilter FileFilter;

/* File type bits. A filter holds a mask of these; a file has exactly one,
 * or FFT_UNKNOWN when its type has not been looked up (yet). */
typedef unsigned int FFType;
enum {
    FFT_UNKNOWN = 0,
    FFT_REG = 1 << 0,
    FFT_DIR = 1 << 1,
    FFT_LNK = 1 << 2,
    FFT_SCK = 1 << 3,
    FFT_PIP = 1 << 4,
    FFT_BLK = 1 << 5,
    FFT_CHR = 1 << 6,
    FFT_ANY = (FFT_REG|FFT_DIR|FFT_LNK|FFT_SCK|FFT_PIP|FFT_BLK|FFT_CHR)
};

typedef enum FFResult {
    FF_NO_MATCH,
    FF_MATCH,
    /* The name matches only filters that are restricted to certain file
     * types, and the type passed in was FFT_UNKNOWN. */
    FF_NEEDS_TYPE
} FFResult;

enum FilterIntent {
    FILTER_LOOKUP,  /* operate on an existing entry */
    FILTER_CREATE   /* create an entry, or replace one, under this name */
};

/* Looks up the type of `path`. Returns 0 and sets *type, or returns a
 * positive errno value. */
typedef int (*filefilter_stat_fn)(void *ctx, const char *path, FFType *type);

FileFilter *filefilter_create(void);
void filefilter_destroy(FileFilter *f); /* NULL is allowed */

bool filefilter_is_empty(const FileFilter *f);

/* True if at least one filter is restricted to certain file types. */
bool filefilter_has_typed(const FileFilter *f);

/*
 * Parses one filter and adds it to the set.
 *
 *   FILTER := [ "type=" TYPE ":" ]... MATCH
 *   MATCH  := "name=" LITERAL | "name-glob=" PATTERN
 *           | "iname=" LITERAL | "iname-glob=" PATTERN
 *   TYPE   := file | dir | symlink | socket | fifo | block | char
 *
 * Parsing stops at the first name key: everything after it is the name,
 * so names may contain ':' and '='. Several "type=" conditions are OR-ed;
 * none means any type. The "i" forms compare case-insensitively (ASCII
 * letters only).
 *
 * Returns false and points *err (if not NULL) at a static message if the
 * spec is invalid. Adding a filter that is already present is not an error.
 */
bool filefilter_add_spec(FileFilter *f, const char *spec, const char **err);

/*
 * Tests a single name. `type` is one FFT_* bit, or FFT_UNKNOWN.
 *
 * Names that libfuse generates for unlinked-but-open files (".fuse_hidden"
 * followed by 16 hex digits) never match, otherwise a filter that covers
 * them would make it impossible to delete an open file.
 */
FFResult filefilter_match(const FileFilter *f, const char *name, FFType type);

/*
 * Checks every component of `path`, which is relative to the root of the
 * filtered tree (leading slashes are ignored).
 *
 * Returns 0 if no component is hidden, otherwise a positive errno value:
 *   ENOENT  a component is hidden;
 *   EPERM   intent is FILTER_CREATE and the final component is hidden, or
 *           would be once created;
 *   other   whatever `stat_fn` returned.
 *
 * `stat_fn` is called with the path up to and including a component, and
 * only when that component's name matches a type-restricted filter. If it
 * is NULL, or cannot determine the type, the component counts as hidden.
 *
 * For FILTER_CREATE the final component is refused if either the existing
 * object or `new_type` (the type about to be created; may be FFT_UNKNOWN
 * if not known) matches. This keeps a hidden object from being replaced
 * and a visible name from turning into a hidden one.
 */
int filefilter_check_path(const FileFilter *f, const char *path,
                          enum FilterIntent intent, FFType new_type,
                          filefilter_stat_fn stat_fn, void *ctx);

/*
 * Decides whether a directory entry must be left out of a listing.
 * `stat_fn` is called with `name`, and only when the type is needed and
 * `type` is FFT_UNKNOWN. If it is NULL or fails, the entry is hidden.
 */
bool filefilter_hides_entry(const FileFilter *f, const char *name, FFType type,
                            filefilter_stat_fn stat_fn, void *ctx);

/* Returns FFT_UNKNOWN for types that filters cannot name. */
FFType filefilter_type_from_mode(mode_t mode);

/* Converts a `d_type` from struct dirent. Returns FFT_UNKNOWN for
 * DT_UNKNOWN and on platforms without d_type. */
FFType filefilter_type_from_dtype(unsigned char d_type);

#endif
