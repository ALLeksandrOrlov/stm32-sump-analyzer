#ifndef SUMP_PROTOCOL_H
#define SUMP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SUMP_RESET = 0x00, SUMP_RUN = 0x01, SUMP_ID = 0x02, SUMP_META = 0x04,
    SUMP_DIVIDER = 0x80, SUMP_COUNTS = 0x81, SUMP_FLAGS = 0x82,
    SUMP_TRIGGER_MASK = 0xc0, SUMP_TRIGGER_VALUE = 0xc1
};

typedef struct { uint8_t opcode; uint32_t value; } sump_command_t;
typedef struct { uint8_t opcode, received; uint32_t value; } sump_parser_t;

void sump_parser_reset(sump_parser_t *parser);
/* Feed every byte, independent of USB packet boundaries. Unknown long commands
 * still consume all four argument bytes. True means one command is complete. */
bool sump_parser_push(sump_parser_t *parser, uint8_t byte, sump_command_t *command);
const uint8_t *sump_identity(size_t *length);
size_t sump_metadata(uint8_t *output, size_t capacity, uint32_t memory,
                     uint32_t rate);

#endif
