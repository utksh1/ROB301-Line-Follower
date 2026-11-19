#ifndef DEBUG_UART_H_
#define DEBUG_UART_H_

#include <stdbool.h>

/*
 * Non-blocking, polling-based UART diagnostic output.
 * Call debug_uart_task() frequently from the main superloop.
 * A single message is buffered at a time.
 */
bool debug_uart_busy(void);
bool debug_uart_printf(const char *format, ...);
void debug_uart_task(void);

#endif /* DEBUG_UART_H_ */
