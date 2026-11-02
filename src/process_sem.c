#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int wh_process_sem_create(WhProcessSem *sem, const char *name,
                          unsigned int value, bool named, sem_t *memory)
{
    memset(sem, 0, sizeof(*sem));
    if (!named && memory != NULL && sem_init(memory, 1, value) == 0)
    {
        sem->handle = memory;
        sem->memory = memory;
        return 0;
    }
    sem->handle = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (sem->handle == SEM_FAILED)
    {
        sem->handle = NULL;
        return -1;
    }
    if (named)
    {
        (void)snprintf(sem->name, sizeof(sem->name), "%s", name);
        sem->owner = true;
    }
    else if (sem_unlink(name) != 0)
    {
        int saved = errno;
        sem_close(sem->handle);
        sem_unlink(name);
        sem->handle = NULL;
        errno = saved;
        return -1;
    }
    return 0;
}

int wh_process_sem_open(WhProcessSem *sem, const char *name)
{
    memset(sem, 0, sizeof(*sem));
    sem->handle = sem_open(name, 0);
    if (sem->handle == SEM_FAILED)
    {
        sem->handle = NULL;
        return -1;
    }
    return 0;
}

int wh_process_sem_close(WhProcessSem *sem)
{
    int result = 0;
    if (sem->memory != NULL)
    {
        if (sem_destroy(sem->memory) != 0)
        {
            result = -1;
        }
    }
    else if (sem->handle != NULL && sem_close(sem->handle) != 0)
    {
        result = -1;
    }
    if (sem->owner && sem_unlink(sem->name) != 0)
    {
        result = -1;
    }
    memset(sem, 0, sizeof(*sem));
    return result;
}

int wh_process_sem_lock(sem_t *sem)
{
    if (sem == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    int result;
    do
    {
        result = sem_wait(sem);
    } while (result != 0 && errno == EINTR);
    return result;
}

int wh_process_sem_unlock(sem_t *sem)
{
    if (sem == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    return sem_post(sem);
}
