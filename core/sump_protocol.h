#ifndef SUMP_PROTOCOL_H
#define SUMP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SUMP_RESET = 0x00,
    SUMP_RUN = 0x01,
    SUMP_ID = 0x02,
    SUMP_META = 0x04,
    SUMP_DIVIDER = 0x80,
    SUMP_COUNTS = 0x81,
    SUMP_FLAGS = 0x82,
    SUMP_TRIGGER_MASK = 0xc0,
    SUMP_TRIGGER_VALUE = 0xc1
};

#define SUMP_LONG_COMMAND_BIT 0x80u
#define SUMP_ARGUMENT_BYTES 4u
#define SUMP_DIVIDER_MASK 0x00ffffffu
#define SUMP_CLOCK_PERIOD_NS 10u
#define SUMP_COUNT_UNIT_SAMPLES 4u

enum {
    SUMP_FLAG_FILTER_HINT = 1u << 1,
    SUMP_FLAG_DISABLE_GROUP_0 = 1u << 2,
    SUMP_FLAG_DISABLE_GROUP_1 = 1u << 3,
    SUMP_FLAG_DISABLE_GROUP_2 = 1u << 4,
    SUMP_FLAG_DISABLE_GROUP_3 = 1u << 5
};

typedef struct {
    uint8_t opcode;
    uint32_t value;
} sump_command_t;

typedef struct {
    uint8_t opcode;
    /* Zero means idle; 1..4 counts the opcode and collected argument bytes. */
    uint8_t received;
    uint32_t value;
} sump_parser_t;

void sump_parser_reset(sump_parser_t *parser);
/* Feed every byte, independent of USB packet boundaries. Unknown long commands
 * still consume all four argument bytes. True means one command is complete. */
bool sump_parser_push(sump_parser_t *parser, uint8_t byte, sump_command_t *command);
const uint8_t *sump_identity(size_t *length);
/* Metadata integers are big-endian, unlike command arguments. The output is
 * unchanged if capacity is insufficient; zero indicates that failure. */
size_t sump_metadata(uint8_t *output, size_t capacity, uint32_t memory,
                     uint32_t rate);

#endif
