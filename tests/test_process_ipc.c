#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char run_id[WH_PROCESS_NAME_SIZE];
    snprintf(run_id, sizeof(run_id), "ipc-%ld", (long)getpid());
    WhProcessShared *first = NULL;
    WhProcessShared *opened = NULL;
    int first_fd = -1;
    int opened_fd = -1;
    int created = 0;
    int status = 1;

    if (wh_process_shm_create(run_id, &first, &first_fd) != 0)
    {
        perror("create shared memory");
        goto cleanup;
    }
    created = 1;
    if (wh_process_shm_create(run_id, &opened, &opened_fd) == 0 || errno != EEXIST)
    {
        fprintf(stderr, "duplicate run id was not rejected\n");
        goto cleanup;
    }
    if (wh_process_shm_open(run_id, &opened, &opened_fd) != 0)
    {
        perror("open existing shared memory");
        goto cleanup;
    }
    if (opened->magic != first->magic)
    {
        fprintf(stderr, "existing shared memory changed\n");
        goto cleanup;
    }
    status = 0;

cleanup:
    wh_process_shm_close(opened_fd, opened);
    if (created && wh_process_shm_unlink(run_id) != 0)
    {
        perror("remove shared memory");
        status = 1;
    }
    wh_process_shm_close(first_fd, first);
    return status;
}
