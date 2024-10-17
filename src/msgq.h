#ifndef MSGQ_H
#define MSGQ_H

#include <zephyr/kernel.h>

// Deklaration der Message Queue
extern struct k_msgq temp_msgq;
extern struct k_msgq press_msgq;
extern struct k_msgq humidity_msgq;

#endif // MSGQ_H