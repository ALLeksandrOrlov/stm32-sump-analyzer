#ifndef ANALYZER_BOARD_H
#define ANALYZER_BOARD_H

#include <stdbool.h>

/* Create the application tasks and hardware adapters before USB is enabled.
 * Start the scheduler only after all generated peripheral setup is complete. */
bool analyzer_board_init(void);
void analyzer_board_start(void);

#endif
