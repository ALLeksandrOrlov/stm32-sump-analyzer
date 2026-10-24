#ifndef ANALYZER_BOARD_H
#define ANALYZER_BOARD_H

#include <stdbool.h>

/**
 * @brief Initialize capture/USB adapters and create all static application tasks.
 * @pre Clocks are configured, USB is not initialized, scheduler is stopped.
 * @return true when every task and communication object is ready.
 */
bool analyzer_board_init(void);

/**
 * @brief Start application scheduling after generated peripheral setup.
 * @pre analyzer_board_init() succeeded and USB hardware is initialized.
 * @note Does not return; scheduler startup failure enters Error_Handler().
 */
void analyzer_board_start(void);

#endif
