#define _XOPEN_SOURCE 700

#include "test_common.h"
#include "filter.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Creates a filter set from a NULL-terminated list of specs. */
static FileFilter *make_filter(const char *const *specs)
{
    FileFilter *f = filefilter_create();
    for (; *specs != NULL; ++specs) {
        const char *err = NULL;
        if (!filefilter_add_spec(f, *specs, &err)) {
            printf("Unexpectedly failed to add \"%s\": %s\n", *specs, err);
            ++failures;
        }
    }
    return f;
}

#define FILTER(...) make_filter((const char *const[]){ __VA_ARGS__, NULL })

static void expect_spec_ok(const char *spec)
{
    FileFilter *f = filefilter_create();
    const char *err = NULL;
    if (!filefilter_add_spec(f, spec, &err)) {
        printf("Expected \"%s\" to be accepted but got: %s\n", spec, err);
        ++failures;
    } else if (filefilter_is_empty(f)) {
        printf("Expected \"%s\" to add a filter\n", spec);
        ++failures;
    }
    filefilter_destroy(f);
}

static void expect_spec_rejected(const char *spec)
{
    FileFilter *f = filefilter_create();
    const char *err = NULL;
    if (filefilter_add_spec(f, spec, &err)) {
        printf("Expected \"%s\" to be rejected\n", spec);
        ++failures;
    } else {
        if (err == NULL || strlen(err) == 0) {
            printf("Expected an error message for \"%s\"\n", spec);
            ++failures;
        }
        if (!filefilter_is_empty(f)) {
            printf("Expected rejected \"%s\" to add nothing\n", spec);
            ++failures;
        }
    }
    filefilter_destroy(f);
}

static void expect_match(const FileFilter *f, const char *name, FFType type, FFResult expected)
{
    FFResult actual = filefilter_match(f, name, type);
    if (actual != expected) {
        printf("Expected filefilter_match(\"%s\", %u) to return %d but got %d\n",
               name, type, (int)expected, (int)actual);
        ++failures;
    }
}


/* A fake filesystem for the stat callback. */

struct fake_file {
    const char *path;
    FFType type;
    int err;  /* if nonzero, the lookup fails with this errno */
};

struct fake_fs {
    const struct fake_file *files;  /* terminated by path == NULL */
    int calls;
    char last_path[256];
};

static int fake_stat(void *ctx, const char *path, FFType *type)
{
    struct fake_fs *fs = ctx;
    ++fs->calls;
    strncpy(fs->last_path, path, sizeof(fs->last_path) - 1);
    fs->last_path[sizeof(fs->last_path) - 1] = '\0';
    for (const struct fake_file *file = fs->files; file->path != NULL; ++file) {
        if (strcmp(file->path, path) == 0) {
            if (file->err != 0)
                return file->err;
            *type = file->type;
            return 0;
        }
    }
    return ENOENT;
}

static void expect_check(const FileFilter *f, const char *path,
                         enum FilterIntent intent, FFType new_type,
                         struct fake_fs *fs, int expected, int expected_calls)
{
    int calls_before = (fs != NULL) ? fs->calls : 0;
    int actual = filefilter_check_path(f, path, intent, new_type,
                                       (fs != NULL) ? fake_stat : NULL, fs);
    if (actual != expected) {
        printf("Expected check_path(\"%s\", %s, %u) to return %d but got %d\n",
               path, intent == FILTER_CREATE ? "CREATE" : "LOOKUP", new_type,
               expected, actual);
        ++failures;
    }
    if (fs != NULL && expected_calls >= 0 && fs->calls - calls_before != expected_calls) {
        printf("Expected check_path(\"%s\") to look up %d type(s) but it looked up %d\n",
               path, expected_calls, fs->calls - calls_before);
        ++failures;
    }
}


static void create_destroy_suite(void)
{
    filefilter_destroy(NULL);

    FileFilter *f = filefilter_create();
    TEST_ASSERT(filefilter_is_empty(f));
    TEST_ASSERT(!filefilter_has_typed(f));
    TEST_ASSERT(filefilter_is_empty(NULL));
    TEST_ASSERT(!filefilter_has_typed(NULL));
    expect_match(f, "anything", FFT_REG, FF_NO_MATCH);
    expect_match(NULL, "anything", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);

    f = FILTER("name=one");
    TEST_ASSERT(!filefilter_is_empty(f));
    TEST_ASSERT(!filefilter_has_typed(f));
    filefilter_destroy(f);

    /* Enough filters to grow the storage several times. */
    f = filefilter_create();
    for (int i = 0; i < 100; ++i) {
        char spec[64];
        snprintf(spec, sizeof(spec), "%sname=file%d", (i % 2) ? "type=dir:" : "", i);
        TEST_ASSERT(filefilter_add_spec(f, spec, NULL));
    }
    TEST_ASSERT(filefilter_has_typed(f));
    expect_match(f, "file0", FFT_REG, FF_MATCH);
    expect_match(f, "file98", FFT_REG, FF_MATCH);
    expect_match(f, "file99", FFT_DIR, FF_MATCH);
    expect_match(f, "file99", FFT_REG, FF_NO_MATCH);
    expect_match(f, "file100", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);
}

static void spec_parsing_suite(void)
{
    expect_spec_ok("name=.zfs");
    expect_spec_ok("name-glob=*.tmp");
    expect_spec_ok("iname=.zfs");
    expect_spec_ok("iname-glob=*.TMP");
    expect_spec_ok("type=file:name=x");
    expect_spec_ok("type=dir:name=x");
    expect_spec_ok("type=symlink:name=x");
    expect_spec_ok("type=socket:name=x");
    expect_spec_ok("type=fifo:name=x");
    expect_spec_ok("type=block:name=x");
    expect_spec_ok("type=char:name=x");
    expect_spec_ok("type=file:type=dir:name-glob=x*");
    expect_spec_ok("type=dir:iname=x");
    expect_spec_ok("type=dir:iname-glob=x*");
    expect_spec_ok("name=with:colon");
    expect_spec_ok("name=with=equals");
    expect_spec_ok("name=x:type=dir");
    expect_spec_ok("name=name=x");
    expect_spec_ok("name=...");
    expect_spec_ok("name=ends\\");
    expect_spec_ok("name-glob=ends\\\\");

    expect_spec_rejected("");
    expect_spec_rejected(".zfs");
    expect_spec_rejected("label=x");
    expect_spec_rejected("NAME=x");
    expect_spec_rejected("name");
    expect_spec_rejected("name=");
    expect_spec_rejected("name-glob=");
    expect_spec_rejected("iname=");
    expect_spec_rejected("iname-glob=");
    expect_spec_rejected("type=dir");
    expect_spec_rejected("type=dir:");
    expect_spec_rejected("type=dir:x");
    expect_spec_rejected("type=:name=x");
    expect_spec_rejected("type=directory:name=x");
    expect_spec_rejected("type=d:name=x");
    expect_spec_rejected("type=file,dir:name=x");
    expect_spec_rejected("path=a/b");
    expect_spec_rejected("name=a/b");
    expect_spec_rejected("name=/a");
    expect_spec_rejected("name-glob=a/*");
    expect_spec_rejected("iname=a/b");
    expect_spec_rejected("name=.");
    expect_spec_rejected("name=..");
    expect_spec_rejected("iname-glob=..");
    expect_spec_rejected("name-glob=ends\\");
    expect_spec_rejected("iname-glob=ends\\\\\\");

    /* A NULL error pointer is allowed. */
    FileFilter *f = filefilter_create();
    TEST_ASSERT(!filefilter_add_spec(f, "bogus", NULL));
    TEST_ASSERT(filefilter_add_spec(f, "name=x", NULL));

    /* Duplicates are accepted without effect. */
    TEST_ASSERT(filefilter_add_spec(f, "name=x", NULL));
    TEST_ASSERT(!filefilter_has_typed(f));
    TEST_ASSERT(filefilter_add_spec(f, "type=dir:type=file:name=y", NULL));
    TEST_ASSERT(filefilter_add_spec(f, "type=file:type=dir:name=y", NULL));
    TEST_ASSERT(filefilter_has_typed(f));
    filefilter_destroy(f);

    /* A very long pattern. */
    size_t long_len = 20000;
    char *long_spec = malloc(long_len + 1);
    memset(long_spec, 'a', long_len);
    memcpy(long_spec, "name=", 5);
    long_spec[long_len] = '\0';
    f = FILTER(long_spec);
    expect_match(f, long_spec + 5, FFT_REG, FF_MATCH);
    expect_match(f, long_spec + 6, FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);
    free(long_spec);
}

static void name_matching_suite(void)
{
    FileFilter *f = FILTER("name=.zfs", "name=x:type=dir", "name=a=b", "name=*");
    expect_match(f, ".zfs", FFT_DIR, FF_MATCH);
    expect_match(f, ".zfs", FFT_REG, FF_MATCH);
    expect_match(f, ".zfs", FFT_UNKNOWN, FF_MATCH);
    expect_match(f, ".ZFS", FFT_DIR, FF_NO_MATCH);
    expect_match(f, ".zfs2", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "zfs", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "", FFT_DIR, FF_NO_MATCH);
    /* Everything after the first name key is the name. */
    expect_match(f, "x:type=dir", FFT_REG, FF_MATCH);
    expect_match(f, "x", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "a=b", FFT_REG, FF_MATCH);
    /* name= is literal, also for wildcard characters. */
    expect_match(f, "*", FFT_REG, FF_MATCH);
    expect_match(f, "anything", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);

    f = FILTER("name-glob=*.tmp", "name-glob=data?", "name-glob=[ab]c", "name-glob=lit\\*");
    expect_match(f, "x.tmp", FFT_REG, FF_MATCH);
    expect_match(f, ".tmp", FFT_REG, FF_MATCH);
    expect_match(f, "x.tmp2", FFT_REG, FF_NO_MATCH);
    expect_match(f, "x.TMP", FFT_REG, FF_NO_MATCH);
    expect_match(f, "data1", FFT_REG, FF_MATCH);
    expect_match(f, "data", FFT_REG, FF_NO_MATCH);
    expect_match(f, "ac", FFT_REG, FF_MATCH);
    expect_match(f, "cc", FFT_REG, FF_NO_MATCH);
    expect_match(f, "lit*", FFT_REG, FF_MATCH);
    expect_match(f, "litx", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);

    /* A wildcard at the start also matches a leading dot. */
    f = FILTER("name-glob=*");
    expect_match(f, ".hidden", FFT_REG, FF_MATCH);
    expect_match(f, "plain", FFT_DIR, FF_MATCH);
    filefilter_destroy(f);
    f = FILTER("name-glob=.*");
    expect_match(f, ".hidden", FFT_REG, FF_MATCH);
    expect_match(f, "plain", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);
}

static void nocase_suite(void)
{
    FileFilter *f = FILTER("iname=.zfs");
    expect_match(f, ".zfs", FFT_DIR, FF_MATCH);
    expect_match(f, ".ZFS", FFT_DIR, FF_MATCH);
    expect_match(f, ".Zfs", FFT_DIR, FF_MATCH);
    expect_match(f, ".zfs2", FFT_DIR, FF_NO_MATCH);
    expect_match(f, ".zf", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "", FFT_DIR, FF_NO_MATCH);
    filefilter_destroy(f);

    f = FILTER("iname=MiXed");
    expect_match(f, "mixed", FFT_REG, FF_MATCH);
    expect_match(f, "MIXED", FFT_REG, FF_MATCH);
    expect_match(f, "MiXed", FFT_REG, FF_MATCH);
    filefilter_destroy(f);

    f = FILTER("iname-glob=*.TMP", "iname-glob=Thumbs.d?");
    expect_match(f, "a.tmp", FFT_REG, FF_MATCH);
    expect_match(f, "A.TMP", FFT_REG, FF_MATCH);
    expect_match(f, "a.Tmp", FFT_REG, FF_MATCH);
    expect_match(f, "a.tmpx", FFT_REG, FF_NO_MATCH);
    expect_match(f, "THUMBS.DB", FFT_REG, FF_MATCH);
    expect_match(f, "thumbs.db", FFT_REG, FF_MATCH);
    filefilter_destroy(f);

    /* The case-sensitive forms stay case-sensitive. */
    f = FILTER("name=.zfs", "name-glob=*.tmp");
    expect_match(f, ".ZFS", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "a.TMP", FFT_REG, FF_NO_MATCH);
    filefilter_destroy(f);

    /* Only ASCII letters are folded. */
    f = FILTER("iname=\xc3\xa4");  /* U+00E4 in UTF-8 */
    expect_match(f, "\xc3\xa4", FFT_REG, FF_MATCH);
    expect_match(f, "\xc3\x84", FFT_REG, FF_NO_MATCH);  /* U+00C4 */
    filefilter_destroy(f);
}

static void typed_matching_suite(void)
{
    static const struct { const char *word; FFType type; } types[] = {
        { "file", FFT_REG }, { "dir", FFT_DIR }, { "symlink", FFT_LNK },
        { "socket", FFT_SCK }, { "fifo", FFT_PIP }, { "block", FFT_BLK },
        { "char", FFT_CHR },
    };
    const size_t num_types = sizeof(types) / sizeof(types[0]);

    for (size_t i = 0; i < num_types; ++i) {
        char spec[64];
        snprintf(spec, sizeof(spec), "type=%s:name=x", types[i].word);
        FileFilter *f = FILTER(spec);
        TEST_ASSERT(filefilter_has_typed(f));
        for (size_t j = 0; j < num_types; ++j) {
            expect_match(f, "x", types[j].type, i == j ? FF_MATCH : FF_NO_MATCH);
        }
        expect_match(f, "x", FFT_UNKNOWN, FF_NEEDS_TYPE);
        expect_match(f, "y", FFT_UNKNOWN, FF_NO_MATCH);
        filefilter_destroy(f);
    }

    FileFilter *f = FILTER("type=file:type=symlink:name-glob=*.png");
    expect_match(f, "a.png", FFT_REG, FF_MATCH);
    expect_match(f, "a.png", FFT_LNK, FF_MATCH);
    expect_match(f, "a.png", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "a.png", FFT_UNKNOWN, FF_NEEDS_TYPE);
    expect_match(f, "a.jpg", FFT_UNKNOWN, FF_NO_MATCH);
    filefilter_destroy(f);

    /* An untyped filter for the same name makes the type irrelevant,
     * whichever order the filters were given in. */
    f = FILTER("type=dir:name=x", "name=x");
    expect_match(f, "x", FFT_UNKNOWN, FF_MATCH);
    expect_match(f, "x", FFT_REG, FF_MATCH);
    filefilter_destroy(f);
    f = FILTER("name=x", "type=dir:name=x");
    expect_match(f, "x", FFT_UNKNOWN, FF_MATCH);
    filefilter_destroy(f);

    /* Several typed filters matching the same name of unknown type. */
    f = FILTER("type=dir:name=x", "type=file:name-glob=x*", "type=fifo:iname=X");
    expect_match(f, "x", FFT_UNKNOWN, FF_NEEDS_TYPE);
    expect_match(f, "x", FFT_REG, FF_MATCH);
    expect_match(f, "x", FFT_LNK, FF_NO_MATCH);
    filefilter_destroy(f);

    /* Typed filters for different types of the same name. */
    f = FILTER("type=dir:name=x", "type=fifo:iname=X");
    expect_match(f, "x", FFT_DIR, FF_MATCH);
    expect_match(f, "x", FFT_PIP, FF_MATCH);
    expect_match(f, "X", FFT_PIP, FF_MATCH);
    expect_match(f, "X", FFT_DIR, FF_NO_MATCH);
    expect_match(f, "x", FFT_REG, FF_NO_MATCH);
    expect_match(f, "X", FFT_UNKNOWN, FF_NEEDS_TYPE);
    filefilter_destroy(f);
}

static void fuse_hidden_suite(void)
{
    FileFilter *f = FILTER("name-glob=*", "name-glob=.*", "type=file:name-glob=.fuse*",
                           "name=.fuse_hidden0000000200000001");
    /* The names libfuse generates are never filtered... */
    expect_match(f, ".fuse_hidden0000000200000001", FFT_REG, FF_NO_MATCH);
    expect_match(f, ".fuse_hidden0000000200000001", FFT_UNKNOWN, FF_NO_MATCH);
    expect_match(f, ".fuse_hiddenabcdef0123456789", FFT_REG, FF_NO_MATCH);
    /* ...but lookalikes are. */
    expect_match(f, ".fuse_hidden", FFT_REG, FF_MATCH);
    expect_match(f, ".fuse_hidden_secret", FFT_REG, FF_MATCH);
    expect_match(f, ".fuse_hidden000000020000000", FFT_REG, FF_MATCH);    /* 15 digits */
    expect_match(f, ".fuse_hidden00000002000000011", FFT_REG, FF_MATCH);  /* 17 digits */
    expect_match(f, ".fuse_hidden000000020000000g", FFT_REG, FF_MATCH);
    expect_match(f, ".fuse_hiddenABCDEF0123456789", FFT_REG, FF_MATCH);
    expect_match(f, "x.fuse_hidden0000000200000001", FFT_REG, FF_MATCH);
    expect_match(f, ".FUSE_HIDDEN0000000200000001", FFT_REG, FF_MATCH);

    TEST_ASSERT(filefilter_check_path(f, "dir", FILTER_LOOKUP, FFT_UNKNOWN, NULL, NULL) == ENOENT);
    TEST_ASSERT(filefilter_check_path(f, ".fuse_hidden0000000200000001",
                                      FILTER_LOOKUP, FFT_UNKNOWN, NULL, NULL) == 0);
    TEST_ASSERT(filefilter_check_path(f, ".fuse_hidden0000000200000001",
                                      FILTER_CREATE, FFT_REG, NULL, NULL) == 0);
    TEST_ASSERT(!filefilter_hides_entry(f, ".fuse_hidden0000000200000001", FFT_REG, NULL, NULL));
    TEST_ASSERT(filefilter_hides_entry(f, ".fuse_hidden_secret", FFT_REG, NULL, NULL));
    filefilter_destroy(f);

    /* fuse-t's NFS "silly rename" names are exempt only with fuse-t. */
    f = FILTER("name-glob=.*");
#ifdef HAVE_FUSE_T
    const FFResult nfs_expected = FF_NO_MATCH;
#else
    const FFResult nfs_expected = FF_MATCH;
#endif
    expect_match(f, ".nfs.20051025.66d9", FFT_REG, nfs_expected);
    expect_match(f, ".nfs.0123abcd.ef01", FFT_UNKNOWN, nfs_expected);
    expect_match(f, ".nfs.secret", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.20051025", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.20051025.66d", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.20051025.66d9a", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.2005102.566d9", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.2005102g.66d9", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.20051025-66d9", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs.20051025.66D9", FFT_REG, FF_MATCH);
    expect_match(f, ".nfs20051025.66d9", FFT_REG, FF_MATCH);
    filefilter_destroy(f);
}

static void check_path_untyped_suite(void)
{
    struct fake_fs fs = { (const struct fake_file[]){ { NULL, 0, 0 } }, 0, "" };
    FileFilter *f = FILTER("name=.zfs", "name-glob=*.secret");

    /* Nothing hidden. Untyped filters never need a type lookup. */
    expect_check(f, "", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "/", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, ".", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "a", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "/a/b/c", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "a/b/c", FILTER_CREATE, FFT_REG, &fs, 0, 0);

    /* Final component. */
    expect_check(f, ".zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/.zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/a/b/.zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/a/x.secret", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/.zfs", FILTER_CREATE, FFT_DIR, &fs, EPERM, 0);
    expect_check(f, "/a/b/.zfs", FILTER_CREATE, FFT_REG, &fs, EPERM, 0);
    expect_check(f, "/a/b/.zfs", FILTER_CREATE, FFT_UNKNOWN, &fs, EPERM, 0);

    /* A hidden parent is "not found", whatever the intent. */
    expect_check(f, "/.zfs/snapshot", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/.zfs/snapshot/x/y", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/a/.zfs/b", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "/.zfs/new", FILTER_CREATE, FFT_REG, &fs, ENOENT, 0);
    expect_check(f, "/a/.zfs/.zfs", FILTER_CREATE, FFT_REG, &fs, ENOENT, 0);

    /* Odd but harmless path shapes. */
    expect_check(f, "//a//b//", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "a//.zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "a/.zfs/", FILTER_CREATE, FFT_DIR, &fs, EPERM, 0);
    expect_check(f, "./a/../.zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "./a/..", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);

    /* Names that merely contain a hidden name are not hidden. */
    expect_check(f, "/a.zfs/.zfsx/x.secrets", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);

    /* Works without a stat callback too. */
    expect_check(f, "/a/b", FILTER_LOOKUP, FFT_UNKNOWN, NULL, 0, -1);
    expect_check(f, "/a/.zfs", FILTER_LOOKUP, FFT_UNKNOWN, NULL, ENOENT, -1);
    filefilter_destroy(f);

    /* An empty filter set hides nothing. */
    f = filefilter_create();
    expect_check(f, "/.zfs", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(NULL, "/.zfs", FILTER_CREATE, FFT_DIR, &fs, 0, 0);
    filefilter_destroy(f);

    /* "." and ".." are never matched, not even by a catch-all. The root
     * stays reachable. */
    f = FILTER("name-glob=*");
    expect_check(f, "", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "/", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, ".", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "./..", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "a", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    expect_check(f, "a", FILTER_CREATE, FFT_REG, &fs, EPERM, 0);
    filefilter_destroy(f);
}

static void check_path_typed_suite(void)
{
    const struct fake_file files[] = {
        { "d", FFT_DIR, 0 },
        { "d/x", FFT_DIR, 0 },
        { "d/x/inner", FFT_REG, 0 },
        { "d/pipe", FFT_PIP, 0 },
        { "d/reg", FFT_REG, 0 },
        { "f", FFT_REG, 0 },
        { "f/x", FFT_REG, 0 },
        { "denied", FFT_UNKNOWN, EACCES },
        { "denied/x", FFT_UNKNOWN, EACCES },
        { "weird", FFT_DIR, 0 },
        { "weird/x", FFT_UNKNOWN, 0 },  /* exists, but of no nameable type */
        { NULL, 0, 0 }
    };
    struct fake_fs fs = { files, 0, "" };
    FileFilter *f = FILTER("type=dir:name=x");

    /* The type is looked up only for a component whose name matches. */
    expect_check(f, "/d/reg", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 0);
    expect_check(f, "/d/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);
    TEST_ASSERT(strcmp(fs.last_path, "d/x") == 0);
    expect_check(f, "/f/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 1);
    TEST_ASSERT(strcmp(fs.last_path, "f/x") == 0);

    /* Non-final component: the lookup gets the path up to that component. */
    expect_check(f, "/d/x/inner", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);
    TEST_ASSERT(strcmp(fs.last_path, "d/x") == 0);
    expect_check(f, "/d/x/new", FILTER_CREATE, FFT_REG, &fs, ENOENT, 1);
    expect_check(f, "/f/x/inner", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 1);

    /* A missing entry is simply not found. */
    expect_check(f, "/d/missing/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);
    expect_check(f, "/missing/x/y", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);

    /* A failing lookup is passed on unchanged, for either intent. */
    expect_check(f, "/denied/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, EACCES, 1);
    expect_check(f, "/denied/x", FILTER_CREATE, FFT_REG, &fs, EACCES, 1);
    expect_check(f, "/denied/x/y", FILTER_LOOKUP, FFT_UNKNOWN, &fs, EACCES, 1);

    /* An object whose type cannot be named counts as hidden. */
    expect_check(f, "/weird/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);
    expect_check(f, "/weird/x", FILTER_CREATE, FFT_REG, &fs, EPERM, 1);

    /* Without a way to look up the type, a typed match counts as hidden. */
    expect_check(f, "/d/x", FILTER_LOOKUP, FFT_UNKNOWN, NULL, ENOENT, -1);
    expect_check(f, "/f/x", FILTER_LOOKUP, FFT_UNKNOWN, NULL, ENOENT, -1);
    expect_check(f, "/f/x", FILTER_CREATE, FFT_REG, NULL, EPERM, -1);
    expect_check(f, "/f/y", FILTER_LOOKUP, FFT_UNKNOWN, NULL, 0, -1);

    /* Creating a name that does not exist yet: the new type decides. */
    expect_check(f, "/d/new/x", FILTER_CREATE, FFT_DIR, &fs, EPERM, 1);
    expect_check(f, "/missing-x", FILTER_CREATE, FFT_DIR, &fs, 0, 0);
    expect_check(f, "/x", FILTER_CREATE, FFT_DIR, &fs, EPERM, 1);
    expect_check(f, "/x", FILTER_CREATE, FFT_REG, &fs, 0, 1);
    expect_check(f, "/x", FILTER_CREATE, FFT_UNKNOWN, &fs, EPERM, 1);
    /* Looking it up is just "not found". */
    expect_check(f, "/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 1);

    /* Replacing: refused if the existing object is hidden... */
    expect_check(f, "/d/x", FILTER_CREATE, FFT_REG, &fs, EPERM, 1);
    expect_check(f, "/d/x", FILTER_CREATE, FFT_UNKNOWN, &fs, EPERM, 1);
    /* ...or if the replacement would be. */
    expect_check(f, "/f/x", FILTER_CREATE, FFT_DIR, &fs, EPERM, 1);
    /* Neither: allowed. */
    expect_check(f, "/f/x", FILTER_CREATE, FFT_REG, &fs, 0, 1);
    expect_check(f, "/f/x", FILTER_CREATE, FFT_UNKNOWN, &fs, 0, 1);
    filefilter_destroy(f);

    /* The two examples from the design: a hidden fifo must not be
     * replaced, and a visible fifo must not become a hidden regular file. */
    f = FILTER("type=fifo:name=pipe");
    expect_check(f, "/d/pipe", FILTER_CREATE, FFT_REG, &fs, EPERM, 1);
    filefilter_destroy(f);
    f = FILTER("type=file:name=pipe");
    expect_check(f, "/d/pipe", FILTER_LOOKUP, FFT_UNKNOWN, &fs, 0, 1);
    expect_check(f, "/d/pipe", FILTER_CREATE, FFT_REG, &fs, EPERM, 1);
    expect_check(f, "/d/pipe", FILTER_CREATE, FFT_PIP, &fs, 0, 1);
    filefilter_destroy(f);

    /* An untyped filter on the same name wins without any lookup. */
    f = FILTER("type=dir:name=x", "name=x");
    expect_check(f, "/f/x", FILTER_LOOKUP, FFT_UNKNOWN, &fs, ENOENT, 0);
    filefilter_destroy(f);
}

static void hides_entry_suite(void)
{
    const struct fake_file files[] = {
        { "x", FFT_DIR, 0 },
        { "a.png", FFT_REG, 0 },
        { "b.png", FFT_DIR, 0 },
        { "broken.png", FFT_UNKNOWN, EIO },
        { "weird.png", FFT_UNKNOWN, 0 },
        { NULL, 0, 0 }
    };
    struct fake_fs fs = { files, 0, "" };
    FileFilter *f = FILTER("name=.zfs", "type=file:name-glob=*.png");

    /* Known type: no lookup. */
    TEST_ASSERT(filefilter_hides_entry(f, ".zfs", FFT_DIR, fake_stat, &fs));
    TEST_ASSERT(filefilter_hides_entry(f, ".zfs", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(filefilter_hides_entry(f, "a.png", FFT_REG, fake_stat, &fs));
    TEST_ASSERT(!filefilter_hides_entry(f, "b.png", FFT_DIR, fake_stat, &fs));
    TEST_ASSERT(!filefilter_hides_entry(f, "plain", FFT_REG, fake_stat, &fs));
    TEST_ASSERT(!filefilter_hides_entry(f, "plain", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(fs.calls == 0);

    /* Unknown type (as with DT_UNKNOWN): falls back to a lookup. */
    TEST_ASSERT(filefilter_hides_entry(f, "a.png", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(fs.calls == 1);
    TEST_ASSERT(strcmp(fs.last_path, "a.png") == 0);
    TEST_ASSERT(!filefilter_hides_entry(f, "b.png", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(fs.calls == 2);

    /* If the lookup fails or is inconclusive, the entry is hidden. */
    TEST_ASSERT(filefilter_hides_entry(f, "broken.png", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(filefilter_hides_entry(f, "vanished.png", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(filefilter_hides_entry(f, "weird.png", FFT_UNKNOWN, fake_stat, &fs));
    TEST_ASSERT(filefilter_hides_entry(f, "a.png", FFT_UNKNOWN, NULL, NULL));

    /* "." and ".." are never hidden. */
    filefilter_destroy(f);
    f = FILTER("name-glob=*", "type=dir:name-glob=.*");
    TEST_ASSERT(!filefilter_hides_entry(f, ".", FFT_DIR, fake_stat, &fs));
    TEST_ASSERT(!filefilter_hides_entry(f, "..", FFT_DIR, fake_stat, &fs));
    TEST_ASSERT(!filefilter_hides_entry(f, "..", FFT_UNKNOWN, NULL, NULL));
    TEST_ASSERT(filefilter_hides_entry(f, "...", FFT_DIR, fake_stat, &fs));
    filefilter_destroy(f);

    /* An empty filter set hides nothing. */
    f = filefilter_create();
    TEST_ASSERT(!filefilter_hides_entry(f, "x", FFT_UNKNOWN, NULL, NULL));
    TEST_ASSERT(!filefilter_hides_entry(NULL, "x", FFT_UNKNOWN, NULL, NULL));
    filefilter_destroy(f);
}

static void type_conversion_suite(void)
{
    TEST_ASSERT(filefilter_type_from_mode(S_IFREG | 0644) == FFT_REG);
    TEST_ASSERT(filefilter_type_from_mode(S_IFDIR | 0755) == FFT_DIR);
    TEST_ASSERT(filefilter_type_from_mode(S_IFLNK | 0777) == FFT_LNK);
    TEST_ASSERT(filefilter_type_from_mode(S_IFSOCK) == FFT_SCK);
    TEST_ASSERT(filefilter_type_from_mode(S_IFIFO) == FFT_PIP);
    TEST_ASSERT(filefilter_type_from_mode(S_IFBLK) == FFT_BLK);
    TEST_ASSERT(filefilter_type_from_mode(S_IFCHR) == FFT_CHR);
    TEST_ASSERT(filefilter_type_from_mode(0) == FFT_UNKNOWN);
    TEST_ASSERT(filefilter_type_from_mode(0644) == FFT_UNKNOWN);

#ifdef DT_UNKNOWN
    TEST_ASSERT(filefilter_type_from_dtype(DT_REG) == FFT_REG);
    TEST_ASSERT(filefilter_type_from_dtype(DT_DIR) == FFT_DIR);
    TEST_ASSERT(filefilter_type_from_dtype(DT_LNK) == FFT_LNK);
    TEST_ASSERT(filefilter_type_from_dtype(DT_SOCK) == FFT_SCK);
    TEST_ASSERT(filefilter_type_from_dtype(DT_FIFO) == FFT_PIP);
    TEST_ASSERT(filefilter_type_from_dtype(DT_BLK) == FFT_BLK);
    TEST_ASSERT(filefilter_type_from_dtype(DT_CHR) == FFT_CHR);
    TEST_ASSERT(filefilter_type_from_dtype(DT_UNKNOWN) == FFT_UNKNOWN);
#endif
    TEST_ASSERT(filefilter_type_from_dtype(255) == FFT_UNKNOWN);

    /* Each type is a distinct single bit within FFT_ANY. */
    const FFType all[] = { FFT_REG, FFT_DIR, FFT_LNK, FFT_SCK, FFT_PIP, FFT_BLK, FFT_CHR };
    FFType seen = 0;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        TEST_ASSERT(all[i] != 0 && (all[i] & (all[i] - 1)) == 0);
        TEST_ASSERT(!(seen & all[i]));
        seen |= all[i];
    }
    TEST_ASSERT(seen == FFT_ANY);
}

static void filter_suite(void)
{
    create_destroy_suite();
    spec_parsing_suite();
    name_matching_suite();
    nocase_suite();
    typed_matching_suite();
    fuse_hidden_suite();
    check_path_untyped_suite();
    check_path_typed_suite();
    hides_entry_suite();
    type_conversion_suite();
}

TEST_MAIN(filter_suite)
