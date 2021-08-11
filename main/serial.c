// Copyright 2020-2021 Espressif Systems (Shanghai) CO LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "esp_timer.h"
#include "util.h"

#define GPIO_BOOT    CONFIG_BRIDGE_GPIO_BOOT
#define GPIO_RST     CONFIG_BRIDGE_GPIO_RST
#define GPIO_RXD     CONFIG_BRIDGE_GPIO_RXD
#define GPIO_TXD     CONFIG_BRIDGE_GPIO_TXD

#define SLAVE_UART_NUM          UART_NUM_1
#define SLAVE_UART_BUF_SIZE     (2 * 1024)

#define USB_SEND_RINGBUFFER_SIZE SLAVE_UART_BUF_SIZE

static const char *TAG = "bridge_serial";

static QueueHandle_t uart_queue;
static RingbufHandle_t usb_sendbuf;

static esp_timer_handle_t state_change_timer;

static bool serial_init_finished = false;
static bool serial_read_enabled = false;

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t dtmp[SLAVE_UART_BUF_SIZE];
    while (1) {
        if (xQueueReceive(uart_queue, (void *) &event, portMAX_DELAY)) {
            switch(event.type) {
                case UART_DATA:
                    if (serial_read_enabled) {
                        const int read = uart_read_bytes(SLAVE_UART_NUM, dtmp, event.size, portMAX_DELAY);
                        ESP_LOGD(TAG, "UART -> CDC ringbuffer (%d bytes)", event.size);

                        // We cannot wait it here because UART events would overflow and have to copy the data into
                        // another buffer and wait until it can be sent.
                        if (xRingbufferSend(usb_sendbuf, dtmp, read, pdMS_TO_TICKS(10)) != pdTRUE) {
                            ESP_LOGW(TAG, "Cannot write to ringbuffer (free %d of %d)!",
                                    xRingbufferGetCurFreeSize(usb_sendbuf),
                                    USB_SEND_RINGBUFFER_SIZE);
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(SLAVE_UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full");
                    uart_flush_input(SLAVE_UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BREAK:
                    ESP_LOGW(TAG, "UART RX break");
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;
                default:
                    ESP_LOGW(TAG, "UART event type: %d", event.type);
                    break;
            }
            taskYIELD();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static void usb_sender_task(void *pvParameters)
{
    while (1) {
        size_t ringbuf_received, try_cnt = 0;
        uint8_t *buf = (uint8_t *) xRingbufferReceiveUpTo(usb_sendbuf, &ringbuf_received, pdMS_TO_TICKS(100),
                CONFIG_USB_CDC_TX_BUFSIZE);

        if (buf) {
            uint8_t int_buf[ringbuf_received];
            memcpy(int_buf, buf, ringbuf_received);
            vRingbufferReturnItem(usb_sendbuf, (void *) buf);

            for (int transferred = 0, to_send = ringbuf_received; transferred < ringbuf_received;) {
                /* try_cnt is used to detect whether usb-cdc buffer is consuming by the host.
                    this will prevent to stuck here when idf monitor is not running */
                if (tud_cdc_write_available() < to_send && ++try_cnt < 10) {
                    tud_cdc_write_flush();
                    ets_delay_us(100);
                    continue;
                }
                const int t = tud_cdc_write(int_buf + transferred, to_send);
                ESP_LOGD(TAG, "CDC ringbuffer -> CDC (%d bytes)", t);
                transferred += t;
                to_send -= t;
                try_cnt = 0;
            }
            tud_cdc_write_flush();
        } else {
            ESP_LOGD(TAG, "usb_sender_task: nothing to send");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
    }
    vTaskDelete(NULL);
}

void tud_cdc_rx_cb(uint8_t itf)
{
    if (!serial_init_finished) {
        // This is a callback function which can be invoked without running start_serial_task()
        ESP_LOGW(TAG, "Tasks for the serial interface hasn't been initialized!");
        return;
    }

    uint8_t buf[CONFIG_USB_CDC_RX_BUFSIZE];
    const uint32_t rx_size = tud_cdc_n_read(itf, buf, CONFIG_USB_CDC_RX_BUFSIZE);
    if (rx_size > 0) {
        ESP_LOGD(TAG, "CDC -> UART (%d bytes)", rx_size);

        const int transferred = uart_write_bytes(SLAVE_UART_NUM, buf, rx_size);
        if (transferred != rx_size) {
            ESP_LOGW(TAG, "uart_write_bytes transferred %d bytes only!", transferred);
        }
    } else {
        ESP_LOGW(TAG, "tud_cdc_rx_cb receive error");
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    if (!serial_init_finished) {
        // This is a callback function which can be invoked without running start_serial_task()
        ESP_LOGW(TAG, "Tasks for the serial interface hasn't been initialized!");
        return;
    }
    // The following transformation of DTR & RTS signals to BOOT & RST is done based on auto reset circutry shown in
    // schematics of ESP boards.

    // defaults for ((dtr && rts) || (!dtr && !rts))
    bool rst = true;
    bool boot = true;

    if (!dtr && rts) {
        rst = false;
        boot = true;
    } else if (dtr && !rts) {
        rst = true;
        boot = false;
    }

    esp_timer_stop(state_change_timer);  // maybe it is not started so not check the exit value

    if (dtr & rts) {
        // The assignment of BOOT=1 and RST=1 is postponed and it is done only if no other state change occurs in time
        // period set by the timer.
        // This is a patch for Esptool. Esptool generates DTR=0 & RTS=1 followed by DTR=1 & RTS=0. However, a callback
        // with DTR = 1 & RTS = 1 is received between. This would prevent to put the target chip into download mode.
        ESP_ERROR_CHECK(esp_timer_start_once(state_change_timer, 10 * 1000 /*us*/));
    } else {
        ESP_LOGI(TAG, "DTR = %d, RTS = %d -> BOOT = %d, RST = %d", dtr, rts, boot, rst);

        gpio_set_level(GPIO_BOOT, boot);
        gpio_set_level(GPIO_RST, rst);
    }
}

static void state_change_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "BOOT = 1, RST = 1");
    gpio_set_level(GPIO_BOOT, true);
    gpio_set_level(GPIO_RST, true);
}

static void init_state_change_timer()
{
    const esp_timer_create_args_t timer_args = {
        .callback = state_change_timer_cb,
        .name = "serial_state_change"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &state_change_timer));
}

void start_serial_task(void *pvParameters)
{
    const loader_esp32_config_t serial_conf = {
        .baud_rate = 115200,
        .uart_port = SLAVE_UART_NUM,
        .uart_rx_pin = GPIO_RXD,
        .uart_tx_pin = GPIO_TXD,
        .rx_buffer_size = SLAVE_UART_BUF_SIZE * 2,
        .tx_buffer_size = 0,
        .uart_queue = &uart_queue,
        .queue_size = 20,
        .reset_trigger_pin = GPIO_RST,
        .gpio0_trigger_pin = GPIO_BOOT,
    };

    if (loader_port_esp32_init(&serial_conf) == ESP_LOADER_SUCCESS) {
        ESP_LOGI(TAG, "UART & GPIO have been initialized");

        gpio_set_level(GPIO_RST, 1);
        gpio_set_level(GPIO_BOOT, 1);

        init_state_change_timer();

        usb_sendbuf = xRingbufferCreate(USB_SEND_RINGBUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);

        if (usb_sendbuf) {
            xTaskCreate(usb_sender_task, "usb_sender_task", 4 * 1024, NULL, 5, NULL);
            xTaskCreate(uart_event_task, "uart_event_task", 8 * 1024, NULL, 5, NULL);
        } else {
            ESP_LOGE(TAG, "Cannot create ringbuffer for USB sender");
            eub_abort();
        }
        serial_init_finished = true;
        serial_read_enabled = true;
    } else {
        ESP_LOGE(TAG, "loader_port_serial_init failed");
        eub_abort();
    }

    vTaskDelete(NULL);
}

void serial_set(bool enable)
{
    serial_read_enabled = enable;
}

bool serial_set_baudrate(int baud)
{
    return uart_set_baudrate(SLAVE_UART_NUM, baud) == ESP_OK;
}