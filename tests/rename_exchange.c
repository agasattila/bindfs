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

/* Atomically exchanges two paths with renameat2(RENAME_EXCHANGE).
 *
 * Exit status: 0 on success, 1 if the call failed (the errno name is
 * printed), 2 on usage error, 3 if the platform has no renameat2. */
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: rename_exchange path1 path2\n");
        return 2;
    }

#if defined(__linux__) && defined(SYS_renameat2)
    const unsigned int rename_exchange = 1 << 1;  /* RENAME_EXCHANGE in <linux/fs.h> */
    if (syscall(SYS_renameat2, AT_FDCWD, argv[1], AT_FDCWD, argv[2], rename_exchange) == -1) {
        switch (errno) {
        case EPERM: printf("EPERM\n"); break;
        case ENOENT: printf("ENOENT\n"); break;
        case EINVAL: printf("EINVAL\n"); break;
        case ENOSYS: printf("ENOSYS\n"); break;
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
