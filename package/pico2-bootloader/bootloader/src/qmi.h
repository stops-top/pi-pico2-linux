#ifndef QMI_H
#define QMI_H

#include <stdint.h>
#include <stddef.h>

size_t setup_psram(void);

void flash_serial_mode(void);
void xip_cache_set(int enable);

#endif /* QMI_H */
