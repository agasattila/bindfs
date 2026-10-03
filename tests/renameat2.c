#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* Calls renameat2() with RENAME_EXCHANGE or RENAME_NOREPLACE.
 *
 * Exit status: 0 on success, 1 if the call failed (the errno name is
 * printed), 2 on usage error, 3 if the platform has no renameat2. */
int main(int argc, char *argv[])
{
    if (argc != 4 || (strcmp(argv[1], "exchange") != 0 && strcmp(argv[1], "noreplace") != 0)) {
        fprintf(stderr, "Usage: renameat2 exchange|noreplace from to\n");
        return 2;
    }

#if defined(__linux__) && defined(SYS_renameat2)
    /* RENAME_NOREPLACE and RENAME_EXCHANGE in <linux/fs.h> */
    unsigned int flags = (strcmp(argv[1], "exchange") == 0) ? (1 << 1) : (1 << 0);
    if (syscall(SYS_renameat2, AT_FDCWD, argv[2], AT_FDCWD, argv[3], flags) == -1) {
        switch (errno) {
        case EEXIST: printf("EEXIST\n"); break;
        case EINVAL: printf("EINVAL\n"); break;
        case ENOENT: printf("ENOENT\n"); break;
        case ENOSYS: printf("ENOSYS\n"); break;
        case EPERM: printf("EPERM\n"); break;
        default: printf("errno %d: %s\n", errno, strerror(errno)); break;
        }
        return 1;
    }
    return 0;
#else
    (void)argv;
    printf("renameat2 not available\n");
    return 3;
#endif
}
