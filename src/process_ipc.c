#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define WH_PROCESS_MAGIC UINT32_C(0x57485233)

int wh_process_name(char *buffer, size_t size, const char *run_id,
                    const char *kind, int robot_id)
{
    if (buffer == NULL || size == 0 || run_id == NULL || kind == NULL ||
        run_id[0] == '\0' || kind[0] == '\0' || robot_id < -1)
    {
        errno = EINVAL;
        return -1;
    }
    for (const char *p = run_id; *p != '\0'; ++p)
    {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
        {
            errno = EINVAL;
            return -1;
        }
    }
    int length = snprintf(buffer, size, "/wh-%s-%s-%d", run_id, kind, robot_id);
    if (length < 0 || (size_t)length >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int shm_path(char *path, size_t size, const char *run_id)
{
    char name[WH_PROCESS_NAME_SIZE];
    if (wh_process_name(name, sizeof(name), run_id, "shm", -1) != 0)
    {
        return -1;
    }
    int length = snprintf(path, size, "/tmp%s", name);
    if (length < 0 || (size_t)length >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int wh_process_shm_create(const char *run_id, WhProcessShared **shared, int *fd)
{
    char path[WH_PROCESS_NAME_SIZE];
    if (shared == NULL || fd == NULL || shm_path(path, sizeof(path), run_id) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    *shared = NULL;
    *fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (*fd < 0)
    {
        return -1;
    }
    if (ftruncate(*fd, (off_t)sizeof(**shared)) != 0)
    {
        int saved = errno;
        close(*fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    *shared = mmap(NULL, sizeof(**shared), PROT_READ | PROT_WRITE, MAP_SHARED,
                   *fd, 0);
    if (*shared == MAP_FAILED)
    {
        int saved = errno;
        *shared = NULL;
        close(*fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    memset(*shared, 0, sizeof(**shared));
    (*shared)->magic = WH_PROCESS_MAGIC;
    return 0;
}

int wh_process_shm_open(const char *run_id, WhProcessShared **shared, int *fd)
{
    char path[WH_PROCESS_NAME_SIZE];
    if (shared == NULL || fd == NULL || shm_path(path, sizeof(path), run_id) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    *shared = NULL;
    *fd = open(path, O_RDWR, 0);
    if (*fd < 0)
    {
        return -1;
    }
    struct stat info;
    if (fstat(*fd, &info) != 0)
    {
        int saved = errno;
        close(*fd);
        errno = saved;
        return -1;
    }
    if (info.st_size != (off_t)sizeof(**shared))
    {
        close(*fd);
        errno = EPROTO;
        return -1;
    }
    *shared = mmap(NULL, sizeof(**shared), PROT_READ | PROT_WRITE, MAP_SHARED,
                   *fd, 0);
    if (*shared == MAP_FAILED)
    {
        int saved = errno;
        *shared = NULL;
        close(*fd);
        errno = saved;
        return -1;
    }
    if ((*shared)->magic != WH_PROCESS_MAGIC)
    {
        munmap(*shared, sizeof(**shared));
        *shared = NULL;
        close(*fd);
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int wh_process_shm_close(int fd, WhProcessShared *shared)
{
    int result = 0;
    if (shared != NULL && munmap(shared, sizeof(*shared)) != 0)
    {
        result = -1;
    }
    if (fd >= 0 && close(fd) != 0)
    {
        result = -1;
    }
    return result;
}

int wh_process_shm_unlink(const char *run_id)
{
    char path[WH_PROCESS_NAME_SIZE];
    if (shm_path(path, sizeof(path), run_id) != 0)
    {
        return -1;
    }
    return unlink(path);
}
