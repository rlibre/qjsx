/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// Pulse trains on the ESP32 (hal_rmt.h), with the RMT driver of ESP-IDF: one
// TX channel per open(), a copy encoder (pulses and reset as RMT symbols) and
// a bytes encoder (sendBytes).
//
// A send:
//   1. copies what it sends (the RMT reads it while it sends), counts its
//      transmissions (bytes, then the reset) and queues them;
//   2. the end of each transmission comes as an interrupt, which only puts
//      the channel number in a queue;
//   3. the event task counts them down; after the last one it frees the
//      copies and calls the done handler.
#include "hal_rmt.h"
#include "memory.h"
#include "platform.h"

#include <stdint.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"

namespace hal {
namespace rmt {

namespace {

// TX channels of the chip and symbols of a memory block: from soc_caps.h when
// this ESP-IDF still has them there, else the values of the ESP32-S3
#ifdef SOC_RMT_TX_CANDIDATES_PER_GROUP
const int CHANNELS = SOC_RMT_TX_CANDIDATES_PER_GROUP;
#else
const int CHANNELS = 4;
#endif
#ifdef SOC_RMT_MEM_WORDS_PER_CHANNEL
const int BLOCK_SYMBOLS = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
const int BLOCK_SYMBOLS = 48;
#endif

const uint32_t MAX_SYMBOL_TICKS = 32767;               // 15 bits per duration in an RMT symbol
const int EVENT_QUEUE_LENGTH = 16;                     // ends of transmission waiting for the task
const int TRANSMIT_QUEUE_DEPTH = 4;                    // transmissions the driver may hold

// An open channel (JS thread, and the event task under channelLock for
// 'pending', 'data' and 'reset').
struct Channel {
    rmt_channel_handle_t handle;        // the TX channel; nullptr: free
    rmt_encoder_handle_t copyEncoder;   // sends RMT symbols as they are
    rmt_encoder_handle_t bytesEncoder;  // sends bytes, a symbol per bit
    void                *data;          // copy of what the send sends (symbols or bytes); owned
    rmt_symbol_word_t   *reset;         // symbols of the final reset; owned; nullptr: none
    int                  pending;       // transmissions of the send not over yet
};

Channel channels[CHANNELS];       // by channel number
platform::Mutex channelLock;      // pending, data, reset of the channels; doneHandler
DoneHandler *doneHandler = nullptr;   // set by setDoneHandler
QueueHandle_t ends = nullptr;     // channels whose transmission ended, from the interrupt
bool started = false;             // queue and event task (JS thread)

/**
 * End of a transmission (interrupt): queues the channel number for the
 * event task. Returns true if a task of higher priority was woken.
 */
bool IRAM_ATTR onTransmitDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *context) {
    int channel = (int)(intptr_t)context;
    BaseType_t higherPriorityWoken = pdFALSE;
    xQueueSendFromISR(ends, &channel, &higherPriorityWoken);
    return higherPriorityWoken == pdTRUE;
}

/**
 * Frees the copies of the send of a channel (channelLock held, or no send
 * in progress).
 */
void releaseCopies(Channel *channel) {
    memory::release(channel->data);
    memory::release(channel->reset);
    channel->data = nullptr;
    channel->reset = nullptr;
}

/**
 * Event task: counts down the transmissions of each send; after the last one
 * of a send, frees its copies and calls the done handler, for ever.
 */
void eventTask(void *) {
    for (;;) {
        int number = -1;
        if (xQueueReceive(ends, &number, portMAX_DELAY) != pdTRUE || number < 0 || number >= CHANNELS) {
            continue;
        }

        // under the lock: one transmission less; the last one ends the send
        channelLock.lock();
        Channel *channel = &channels[number];
        bool sendOver = false;
        if (channel->pending > 0) {
            channel->pending--;
            sendOver = channel->pending == 0;
        }
        if (sendOver) {
            releaseCopies(channel);
        }
        DoneHandler *handler = doneHandler;
        channelLock.unlock();

        // tell the module
        if (sendOver && handler != nullptr) {
            handler(number);
        }
    }
}

/**
 * Starts, once, the queue and the event task. False if one cannot start.
 */
bool startEvents() {
    if (started) {
        return true;
    }

    ends = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(int));
    if (ends == nullptr) {
        return false;
    }
    if (!platform::startThread(&eventTask, nullptr, platform::defaultThreadOptions("rmt-events"))) {
        return false;
    }

    started = true;
    return true;
}

/**
 * Number of pieces of at most MAX_SYMBOL_TICKS that 'ticks' needs (at least
 * one).
 */
size_t piecesOf(uint32_t ticks) {
    size_t pieces = (ticks + MAX_SYMBOL_TICKS - 1) / MAX_SYMBOL_TICKS;
    return pieces == 0 ? 1 : pieces;
}

/**
 * Packs pulses into RMT symbols, two durations per symbol, a long pulse cut
 * into pieces of at most MAX_SYMBOL_TICKS. Returns the symbols (allocated)
 * and their number in *symbolCount. An odd number of pieces ends with a zero
 * duration, which ends the transmission.
 */
rmt_symbol_word_t *packPulses(const Pulse *pulses, size_t count, size_t *symbolCount) {
    // count the pieces, then the symbols
    size_t pieces = 0;
    for (size_t i = 0; i < count; i++) {
        pieces += piecesOf(pulses[i].ticks);
    }
    *symbolCount = (pieces + 1) / 2;
    rmt_symbol_word_t *symbols = (rmt_symbol_word_t *)memory::allocate(sizeof(rmt_symbol_word_t) * (*symbolCount + 1));
    memset(symbols, 0, sizeof(rmt_symbol_word_t) * (*symbolCount + 1));

    // write each piece in the first or the second half of a symbol
    size_t piece = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t left = pulses[i].ticks;
        int level = pulses[i].level ? 1 : 0;
        do {
            uint32_t ticks = left > MAX_SYMBOL_TICKS ? MAX_SYMBOL_TICKS : left;
            rmt_symbol_word_t *symbol = &symbols[piece / 2];
            if (piece % 2 == 0) {
                symbol->duration0 = ticks;
                symbol->level0 = level;
            } else {
                symbol->duration1 = ticks;
                symbol->level1 = level;
            }
            left -= ticks;
            piece++;
        } while (left > 0);
    }
    return symbols;
}

/**
 * The channel, if open; nullptr otherwise.
 */
Channel *openChannel(int number) {
    if (number < 0 || number >= CHANNELS || channels[number].handle == nullptr) {
        return nullptr;
    }
    return &channels[number];
}

/**
 * Starts a send of 'transmissions' transmissions with these copies (owned
 * from now on). False, copies freed, if a send is in progress.
 */
bool beginSend(Channel *channel, void *data, rmt_symbol_word_t *reset, int transmissions) {
    channelLock.lock();
    bool busy = channel->pending > 0;
    if (!busy) {
        channel->data = data;
        channel->reset = reset;
        channel->pending = transmissions;
    }
    channelLock.unlock();

    if (busy) {
        memory::release(data);
        memory::release(reset);
    }
    return !busy;
}

/**
 * A transmission could not be queued: the send is shortened by
 * 'notQueued' transmissions; if none is left, it is over (copies freed, no
 * done: the caller reports the failure).
 */
void cancelTransmissions(Channel *channel, int notQueued) {
    channelLock.lock();
    channel->pending -= notQueued;
    if (channel->pending <= 0) {
        channel->pending = 0;
        releaseCopies(channel);
    }
    channelLock.unlock();
}

} // namespace

/**
 * The TX channels of the chip.
 */
int channelCount() {
    return CHANNELS;
}

/**
 * A free slot, its TX channel on the pin, its two encoders and its callback.
 */
int open(int pin, uint32_t resolutionHz) {
    if (!startEvents()) {
        return -1;
    }

    // a free slot
    int number = 0;
    while (number < CHANNELS && channels[number].handle != nullptr) {
        number++;
    }
    if (number == CHANNELS) {
        return -1;
    }
    Channel *channel = &channels[number];

    // the TX channel: the pin, the resolution, one block of symbols
    rmt_tx_channel_config_t config = {};
    config.gpio_num = (gpio_num_t)pin;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = resolutionHz;
    config.mem_block_symbols = BLOCK_SYMBOLS;
    config.trans_queue_depth = TRANSMIT_QUEUE_DEPTH;
    if (rmt_new_tx_channel(&config, &channel->handle) != ESP_OK) {
        channel->handle = nullptr;
        return -1;
    }

    // the encoders, the end-of-transmission callback, then enable it
    rmt_copy_encoder_config_t copyConfig = {};
    rmt_bytes_encoder_config_t bytesConfig = {};
    bytesConfig.flags.msb_first = 1;
    rmt_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_done = &onTransmitDone;
    bool ok = rmt_new_copy_encoder(&copyConfig, &channel->copyEncoder) == ESP_OK
           && rmt_new_bytes_encoder(&bytesConfig, &channel->bytesEncoder) == ESP_OK
           && rmt_tx_register_event_callbacks(channel->handle, &callbacks, (void *)(intptr_t)number) == ESP_OK
           && rmt_enable(channel->handle) == ESP_OK;
    if (!ok) {
        close(number);
        return -1;
    }
    return number;
}

/**
 * Disables and deletes the channel and its encoders; a send in progress is
 * dropped, its copies freed.
 */
void close(int number) {
    if (number < 0 || number >= CHANNELS || channels[number].handle == nullptr) {
        return;
    }
    Channel *channel = &channels[number];

    // stop the hardware, then free everything
    rmt_disable(channel->handle);
    if (channel->copyEncoder != nullptr) {
        rmt_del_encoder(channel->copyEncoder);
    }
    if (channel->bytesEncoder != nullptr) {
        rmt_del_encoder(channel->bytesEncoder);
    }
    rmt_del_channel(channel->handle);

    channelLock.lock();
    releaseCopies(channel);
    *channel = Channel{ nullptr, nullptr, nullptr, nullptr, nullptr, 0 };
    channelLock.unlock();
}

/**
 * One handler for every channel.
 */
void setDoneHandler(DoneHandler *handler) {
    channelLock.lock();
    doneHandler = handler;
    channelLock.unlock();
}

/**
 * The pulses as symbols, through the copy encoder.
 */
bool sendPulses(int number, const Pulse *pulses, size_t count) {
    Channel *channel = openChannel(number);
    if (channel == nullptr || count == 0) {
        return false;
    }

    // the symbols, owned by the send
    size_t symbolCount = 0;
    rmt_symbol_word_t *symbols = packPulses(pulses, count, &symbolCount);
    if (!beginSend(channel, symbols, nullptr, 1)) {
        return false;
    }

    // queue the transmission, low at the end
    rmt_transmit_config_t transmit = {};
    transmit.flags.eot_level = 0;
    if (rmt_transmit(channel->handle, channel->copyEncoder, symbols, symbolCount * sizeof(rmt_symbol_word_t), &transmit) != ESP_OK) {
        cancelTransmissions(channel, 1);
        return false;
    }
    return true;
}

/**
 * The bytes through the bytes encoder (its bits set for this send), then the
 * reset as low symbols through the copy encoder.
 */
bool sendBytes(int number, const uint8_t *bytes, size_t size, BitTiming bit0, BitTiming bit1, uint32_t resetTicks) {
    Channel *channel = openChannel(number);
    if (channel == nullptr || size == 0) {
        return false;
    }

    // the bits of this send (no send in progress: checked by beginSend below,
    // the encoder is not in use)
    channelLock.lock();
    bool busy = channel->pending > 0;
    channelLock.unlock();
    if (busy) {
        return false;
    }

    rmt_bytes_encoder_config_t bytesConfig = {};
    bytesConfig.bit0.duration0 = bit0.high;
    bytesConfig.bit0.level0 = 1;
    bytesConfig.bit0.duration1 = bit0.low;
    bytesConfig.bit0.level1 = 0;
    bytesConfig.bit1.duration0 = bit1.high;
    bytesConfig.bit1.level0 = 1;
    bytesConfig.bit1.duration1 = bit1.low;
    bytesConfig.bit1.level1 = 0;
    bytesConfig.flags.msb_first = 1;
    if (rmt_bytes_encoder_update_config(channel->bytesEncoder, &bytesConfig) != ESP_OK) {
        return false;
    }

    // the copies: the bytes, and the reset as a low pulse
    uint8_t *data = (uint8_t *)memory::allocate(size);
    memcpy(data, bytes, size);
    size_t resetCount = 0;
    rmt_symbol_word_t *reset = nullptr;
    if (resetTicks > 0) {
        Pulse low = { resetTicks, 0 };
        reset = packPulses(&low, 1, &resetCount);
    }
    int transmissions = reset != nullptr ? 2 : 1;
    if (!beginSend(channel, data, reset, transmissions)) {
        return false;
    }

    // queue the bytes, then the reset
    rmt_transmit_config_t transmit = {};
    transmit.flags.eot_level = 0;
    if (rmt_transmit(channel->handle, channel->bytesEncoder, data, size, &transmit) != ESP_OK) {
        cancelTransmissions(channel, transmissions);
        return false;
    }
    if (reset != nullptr) {
        if (rmt_transmit(channel->handle, channel->copyEncoder, reset, resetCount * sizeof(rmt_symbol_word_t), &transmit) != ESP_OK) {
            cancelTransmissions(channel, 1);
        }
    }
    return true;
}

} // namespace rmt
} // namespace hal
