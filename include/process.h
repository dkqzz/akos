#ifndef WH_PROCESS_H
#define WH_PROCESS_H

#include "warehouse.h"

#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>

#define WH_PROCESS_NAME_SIZE 64

typedef struct
{
    uint32_t magic;
    int ready[WH_MAX_ROBOTS];
    sem_t unnamed[WH_MAX_ROBOTS * 2];
    Warehouse warehouse;
} WhProcessShared;

typedef struct
{
    sem_t *handle;
    sem_t *memory;
    bool owner;
    char name[WH_PROCESS_NAME_SIZE];
} WhProcessSem;

int wh_process_name(char *buffer, size_t size, const char *run_id,
                    const char *kind, int robot_id);
int wh_process_shm_create(const char *run_id, WhProcessShared **shared, int *fd);
int wh_process_shm_open(const char *run_id, WhProcessShared **shared, int *fd);
int wh_process_shm_close(int fd, WhProcessShared *shared);
int wh_process_shm_unlink(const char *run_id);
int wh_process_sem_create(WhProcessSem *sem, const char *name,
                          unsigned int value, bool named, sem_t *memory);
int wh_process_sem_open(WhProcessSem *sem, const char *name);
int wh_process_sem_close(WhProcessSem *sem);
int wh_process_sem_lock(sem_t *sem);
int wh_process_sem_unlock(sem_t *sem);
int wh_process_wait(sem_t *sem, unsigned int timeout_ms, bool stop_on_signal);
int wh_process_run(Warehouse *warehouse, int max_ticks, int delay_ms,
                   sem_t **wake, sem_t **done, int count,
                   sem_t *state_lock);
void wh_process_worker(WhProcessShared *shared, int index, sem_t *wake,
                       sem_t *done, sem_t *state_lock, sem_t *output_lock,
                       int log_fd);
int wh_run_process_parent(Warehouse *warehouse, int max_ticks, int delay_ms,
                          bool named_semaphores);

#endif
