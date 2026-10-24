/**
 * @file sump_protocol.h
 * @brief Incremental SUMP framing and fixed device responses.
 *
 * Framing depends only on the opcode's high bit. USB packet boundaries and
 * pauses between bytes have no meaning to this parser. Command arguments are
 * little-endian; numeric metadata records are big-endian.
 */
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

#define SUMP_LONG_COMMAND_BIT 0x80U
#define SUMP_ARGUMENT_BYTES 4U
#define SUMP_DIVIDER_MASK UINT32_C(0x00ffffff)
#define SUMP_CLOCK_PERIOD_NS 10U
#define SUMP_COUNT_UNIT_SAMPLES 4U
#define SUMP_METADATA_MAX_BYTES 64U

#define SUMP_FLAG_FILTER_HINT (UINT32_C(1) << 1U)
#define SUMP_FLAG_DISABLE_GROUP_0 (UINT32_C(1) << 2U)
#define SUMP_FLAG_DISABLE_GROUP_1 (UINT32_C(1) << 3U)
#define SUMP_FLAG_DISABLE_GROUP_2 (UINT32_C(1) << 4U)
#define SUMP_FLAG_DISABLE_GROUP_3 (UINT32_C(1) << 5U)

/** Completed frame. Short commands always have value zero. */
typedef struct {
    uint8_t opcode;
    uint32_t value;
} sump_command_t;

/** Parser storage owned exclusively by one task. Initialize before use. */
typedef struct {
    uint8_t opcode;
    uint8_t received; /**< Zero: idle; 1..4: opcode plus collected argument bytes. */
    uint32_t value;
} sump_parser_t;

/** @brief Discard an incomplete frame and return to the opcode state. */
void sump_parser_reset(sump_parser_t *parser);

/**
 * @brief Consume exactly one byte, including payloads of unknown commands.
 * @return true only when command receives a complete frame.
 * @post command is unchanged when the return value is false.
 * @pre parser and command are non-NULL, and parser has been initialized.
 */
bool sump_parser_push(sump_parser_t *parser, uint8_t byte, sump_command_t *command);

/** @brief Return immutable SUMP identification bytes and their length. */
const uint8_t *sump_identity(size_t *length);

/**
 * @brief Encode device metadata without allocating storage.
 * @param output Caller-owned destination; must not be NULL.
 * @param capacity Destination size; SUMP_METADATA_MAX_BYTES is always sufficient.
 * @param memory Capture capacity in samples, one byte per sample.
 * @param rate Maximum advertised sample rate in hertz.
 * @return Encoded length, or zero without modifying output if it does not fit.
 */
size_t sump_metadata(uint8_t *output, size_t capacity, uint32_t memory,
                     uint32_t rate);

#endif
