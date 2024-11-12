#ifndef APP_MUTEX_H
#define APP_MUTEX_H

#include <zephyr/kernel.h>

extern struct k_mutex temp_mutex;
extern struct k_mutex press_mutex;
extern struct k_mutex humidity_mutex;

#endif // MSGQ_H