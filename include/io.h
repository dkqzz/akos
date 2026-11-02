#ifndef WH_IO_H
#define WH_IO_H

#include "warehouse.h"

#include <semaphore.h>

void wh_output_bind(Warehouse *warehouse, int output_fd, int log_fd,
                    bool quiet, sem_t *lock);
void wh_output_unbind(void);

#endif
