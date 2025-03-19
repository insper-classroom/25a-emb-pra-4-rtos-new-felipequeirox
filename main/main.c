#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>
#include "ssd1306.h"
#include "gfx.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/timer.h"

#define TRIGGER_PIN 17
#define ECHO_PIN 16

QueueHandle_t xQueueTime;
QueueHandle_t xQueueDistance;
QueueHandle_t xQueueFail;
SemaphoreHandle_t xSemaphoreTrigger;

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    bool fail = true;
    xQueueSendFromISR(xQueueFail, &fail, NULL);
    return 0;
}

void pin_callback(uint gpio, uint32_t events) {
    static QueueHandle_t xQueueStartEcho;
    static QueueHandle_t xQueueAlarmId;

    uint32_t end_echo, duration;
    alarm_id_t alarm_id;
    uint32_t start_echo;

    if (xQueueStartEcho == NULL) {
        xQueueStartEcho = xQueueCreate(1, sizeof(uint32_t));
        xQueueAlarmId = xQueueCreate(1, sizeof(alarm_id_t));
    }

    if (events & GPIO_IRQ_EDGE_RISE) {
        start_echo = time_us_32();
        xQueueSendFromISR(xQueueStartEcho, &start_echo, NULL);

        bool fail = false;
        xQueueSendFromISR(xQueueFail, &fail, NULL);

        if (xQueueReceive(xQueueAlarmId, &alarm_id, 0)) {
            cancel_alarm(alarm_id);
        }
        alarm_id = add_alarm_in_ms(100, alarm_callback, NULL, false);
        xQueueSendFromISR(xQueueAlarmId, &alarm_id, NULL);
    } 
    else if (events & GPIO_IRQ_EDGE_FALL) {
        if (xQueueReceive(xQueueStartEcho, &start_echo, 0)) {
            end_echo = time_us_32();
            duration = end_echo - start_echo;
            xQueueSendFromISR(xQueueTime, &duration, NULL);
        }

        if (xQueueReceive(xQueueAlarmId, &alarm_id, 0)) {
            cancel_alarm(alarm_id);
        }
    }
}

void trigger_task(void *p) {
    while (1) {
        gpio_put(TRIGGER_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(15));
        gpio_put(TRIGGER_PIN, 0);
        xSemaphoreGive(xSemaphoreTrigger);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void echo_task(void *p) {
    uint32_t echo_time;
    float dist;
    
    while (1) {
        if (xQueueReceive(xQueueTime, &echo_time, portMAX_DELAY)) {
            dist = (echo_time * 0.034) / 2;

            bool fail = (dist > 400 || dist < 2);
            xQueueSend(xQueueFail, &fail, portMAX_DELAY);

            if (!fail) {
                xQueueSend(xQueueDistance, &dist, portMAX_DELAY);
            }
        }
    }
}

void oled_task(void *p) {
    ssd1306_t disp;
    ssd1306_init();
    gfx_init(&disp, 128, 32);

    float dist;
    bool fail;
    while (1) {
        if (xQueueReceive(xQueueDistance, &dist, pdMS_TO_TICKS(200))) {
            gfx_clear_buffer(&disp);
            char msg[20];
            sprintf(msg, "Dist: %.2f cm", dist);
            gfx_draw_string(&disp, 0, 0, 1, msg);
            int bar_length = (int)(dist / 2);
            if (bar_length > 128) bar_length = 128;
            gfx_draw_line(&disp, 0, 10, bar_length, 10);
            gfx_show(&disp);
        } else if (xQueueReceive(xQueueFail, &fail, 0) && fail) {
            gfx_clear_buffer(&disp);
            gfx_draw_string(&disp, 0, 0, 1, "Falha na leitura");
            gfx_show(&disp);
        }
    }
}

int main() {
    stdio_init_all();

    gpio_init(TRIGGER_PIN);
    gpio_init(ECHO_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);
    gpio_set_dir(ECHO_PIN, GPIO_IN);
    gpio_put(TRIGGER_PIN, 0);
    gpio_set_irq_enabled_with_callback(ECHO_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &pin_callback);

    xQueueTime = xQueueCreate(10, sizeof(uint32_t));
    xQueueDistance = xQueueCreate(10, sizeof(float));
    xQueueFail = xQueueCreate(10, sizeof(bool));
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger Task", 256, NULL, 1, NULL);
    xTaskCreate(echo_task, "Echo Task", 256, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED Task", 256, NULL, 1, NULL);

    vTaskStartScheduler();
    while (1);
}
