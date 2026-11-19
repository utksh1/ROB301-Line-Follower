#include "debug_uart.h"

#include "main.h"       /* HAL types, register and UART flag definitions */
#include <stdarg.h>     /* va_list, va_start, va_end */
#include <stdio.h>      /* vsnprintf */
#include <stdint.h>     /* uint8_t, uint16_t */

/* Defined once by STM32CubeIDE (usually in main.c or usart.c). */
#define huart	huart1
extern UART_HandleTypeDef huart;

#define DEBUG_TX_SIZE 128U

static char debug_tx[DEBUG_TX_SIZE];
static uint16_t debug_tx_length = 0U;
static uint16_t debug_tx_index = 0U;

bool debug_uart_busy(void)
{
    return debug_tx_length != 0U;
}

bool debug_uart_printf(const char *format, ...)
{
    if (debug_uart_busy() || (format == NULL))
    {
        return false;
    }

    va_list args;
    va_start(args, format);
    int length = vsnprintf(debug_tx, sizeof(debug_tx), format, args);
    va_end(args);

    if (length < 0)
    {
        return false;
    }

    /* vsnprintf() returns the number of bytes needed, excluding '\0'.
       Clamp to the number actually written when truncated. */
    debug_tx_length = (uint16_t)((length < (int)DEBUG_TX_SIZE)
                              ? length
                              : ((int)DEBUG_TX_SIZE - 1));
    debug_tx_index = 0U;
    return true;
}

void debug_uart_task(void)
{
    if (debug_tx_index < debug_tx_length)
    {
        if (__HAL_UART_GET_FLAG(&huart, UART_FLAG_TXFNF))
        {
            huart.Instance->TDR = (uint8_t)debug_tx[debug_tx_index++];
        }
    }
    else if ((debug_tx_length != 0U) &&
             __HAL_UART_GET_FLAG(&huart, UART_FLAG_TC))
    {
        debug_tx_length = 0U;
        debug_tx_index = 0U;
    }
}
