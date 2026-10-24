/* Real analyzer application over a PTY, with deterministic synthetic capture.
 * This exercises PulseView/SUMP interoperability, not STM32 peripherals or USB. */
#define _DEFAULT_SOURCE
#include "analyzer_app.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

enum { TX_QUEUE_PACKETS = 3 };

typedef struct {
    uint8_t bytes[ANALYZER_TRANSFER_BYTES];
    size_t length;
} tx_packet_t;

typedef struct {
    int serial_fd;
    uint8_t samples[CAPTURE_BUFFER_SAMPLES];
    uint32_t sample_count;
    capture_hw_event_t event;
    tx_packet_t tx_queue[TX_QUEUE_PACKETS];
    size_t tx_head;
    size_t tx_count;
    size_t tx_offset;
} fixture_t;

/* One 400-sample I2C write: START, address 0x50, bytes 0x10/0x2a, STOP.
 * D0 = SCL, D1 = SDA. Each bit takes 10 samples; all other channels are low. */
static uint8_t i2c_sample(uint32_t sample)
{
    sample %= 400;
    if (sample < 40) {
        return 3;
    }
    if (sample < 45) {
        return 1; /* START: SDA falls while SCL is high. */
    }
    if (sample < 315) {
        static const uint8_t bytes[] = {0xa0, 0x10, 0x2a};
        uint32_t bit = (sample - 45) / 10;
        uint32_t phase = (sample - 45) % 10;
        bool sda = bit % 9 == 8 ? false :
            ((bytes[bit / 9] >> (7 - bit % 9)) & 1u) != 0;
        return (uint8_t)((phase >= 5 ? 1 : 0) | (sda ? 2 : 0));
    }
    if (sample < 320) {
        return 0;
    }
    if (sample < 325) {
        return 1;
    }
    return 3; /* STOP: SDA rises while SCL is high. */
}

static bool capture_start(void *context, const capture_plan_t *plan)
{
    fixture_t *fixture = context;
    fixture->sample_count = plan->config.samples;
    for (uint32_t i = 0; i < fixture->sample_count; ++i) {
        fixture->samples[i] = i2c_sample(i);
    }
    fixture->event = CAPTURE_HW_COMPLETE;
    fprintf(stderr, "Synthetic capture: %u samples, period %u ns\n",
            fixture->sample_count, plan->config.sample_period_ns);
    return true;
}
static void capture_stop(void *context)
{
    ((fixture_t *)context)->event = CAPTURE_HW_NONE;
}
static capture_hw_event_t capture_event(void *context)
{
    fixture_t *fixture = context;
    capture_hw_event_t event = fixture->event;
    fixture->event = CAPTURE_HW_NONE;
    return event;
}
static const uint8_t *capture_buffer(void *context, uint32_t *next_write)
{
    fixture_t *fixture = context;
    *next_write = fixture->sample_count;
    return fixture->samples;
}
static bool transport_idle(void *context)
{
    fixture_t *fixture = context;
    return fixture->tx_count == 0;
}

static bool transport_write(void *context, const uint8_t *bytes, size_t length)
{
    fixture_t *fixture = context;
    if (length == 0 || length > ANALYZER_TRANSFER_BYTES ||
        fixture->tx_count == TX_QUEUE_PACKETS) {
        return false;
    }
    size_t tail = (fixture->tx_head + fixture->tx_count) % TX_QUEUE_PACKETS;
    tx_packet_t *packet = &fixture->tx_queue[tail];
    memcpy(packet->bytes, bytes, length);
    packet->length = length;
    ++fixture->tx_count;
    return true;
}

static void transport_cancel(void *context)
{
    fixture_t *fixture = context;
    /* Finish a partially written packet; later queued packets are discarded. */
    fixture->tx_count = fixture->tx_offset > 0 ? 1 : 0;
}

static bool flush_transport(fixture_t *fixture)
{
    if (fixture->tx_count == 0) {
        return true;
    }
    tx_packet_t *packet = &fixture->tx_queue[fixture->tx_head];
    ssize_t written = write(fixture->serial_fd, packet->bytes + fixture->tx_offset,
                            packet->length - fixture->tx_offset);
    if (written < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    fixture->tx_offset += (size_t)written;
    if (fixture->tx_offset == packet->length) {
        fixture->tx_head = (fixture->tx_head + 1) % TX_QUEUE_PACKETS;
        --fixture->tx_count;
        fixture->tx_offset = 0;
    }
    return true;
}

int main(void)
{
    static fixture_t fixture;
    analyzer_app_t app;
    int slave;
    char slave_name[128];
    if (openpty(&fixture.serial_fd, &slave, slave_name, NULL, NULL) != 0) {
        perror("openpty");
        return 1;
    }
    struct termios settings;
    if (tcgetattr(slave, &settings) != 0) {
        perror("tcgetattr");
        return 1;
    }
    cfmakeraw(&settings);
    if (tcsetattr(slave, TCSANOW, &settings) != 0 ||
        fcntl(fixture.serial_fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("configure PTY");
        return 1;
    }
    const char *port = "/tmp/stm32-sump-analyzer";
    unlink(port);
    if (symlink(slave_name, port) != 0) {
        perror("symlink");
        return 1;
    }
    fprintf(stderr, "STM32 SUMP Analyzer synthetic device: %s\n", port);
    analyzer_app_init(&app,
        (capture_driver_t){
            .context = &fixture, .start = capture_start, .stop = capture_stop,
            .poll = capture_event, .buffer = capture_buffer
        },
        (analyzer_transport_t){
            .context = &fixture, .write = transport_write,
            .idle = transport_idle, .cancel = transport_cancel
        }, 168000000);
    for (;;) {
        struct pollfd fd = {
            .fd = fixture.serial_fd,
            .events = POLLIN | (fixture.tx_count > 0 ? POLLOUT : 0)
        };
        if (poll(&fd, 1, 1) < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }
        uint8_t byte;
        if (!app.reply_length && read(fixture.serial_fd, &byte, 1) == 1) {
            if (app.parser.received == 0 && byte == SUMP_RUN) {
                fprintf(stderr, "RUN: samples=%u post=%u period_ns=%u flags=0x%x\n",
                        app.config.samples, app.config.post_trigger,
                        app.config.sample_period_ns, app.sump_flags);
            }
            analyzer_app_receive(&app, byte);
        }
        analyzer_app_poll(&app);
        if (!flush_transport(&fixture)) {
            perror("write PTY");
            return 1;
        }
    }
}
