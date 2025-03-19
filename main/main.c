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
SemaphoreHandle_t xSemaphoreTrigger;

volatile bool fail = false;
volatile uint32_t start_echo = 0;
volatile float distance = 0.0;
volatile alarm_id_t alarm_id = -1;

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    fail = true;
    alarm_id = -1;
    return 0;
}

void pin_callback(uint gpio, uint32_t events) {
    uint32_t end_echo, duration;
    if (events & GPIO_IRQ_EDGE_RISE) {
        start_echo = time_us_32();
        fail = false;
        if (alarm_id != -1) {
            cancel_alarm(alarm_id);
        }
        alarm_id = add_alarm_in_ms(100, alarm_callback, NULL, false);
    } else if (events & GPIO_IRQ_EDGE_FALL) {
        end_echo = time_us_32();
        if (start_echo > 0) {
            duration = end_echo - start_echo;
            distance = (duration * 0.034) / 2;
            xQueueSendFromISR(xQueueDistance, &distance, NULL);
        }
        if (alarm_id != -1) {
            cancel_alarm(alarm_id);
            alarm_id = -1;
        }
    }
}

void trigger_task(void *p) {
    while (1) {
        gpio_put(TRIGGER_PIN, 1);
        sleep_us(15);
        gpio_put(TRIGGER_PIN, 0);
        xSemaphoreGive(xSemaphoreTrigger);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void echo_task(void *p) {
    uint32_t echo_time;
    while (1) {
        if (xQueueReceive(xQueueTime, &echo_time, portMAX_DELAY)) {
            float dist = (echo_time * 0.034) / 2;
            xQueueSend(xQueueDistance, &dist, portMAX_DELAY);
        }
    }
}

void oled_task(void *p) {
    ssd1306_t disp;
    ssd1306_init();
    gfx_init(&disp, 128, 32);

    float dist;
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
        } else if (fail) {
            gfx_clear_buffer(&disp);
            gfx_draw_string(&disp, 0, 0, 1, "Falha na leitura");
            gfx_show(&disp);
            fail = false;
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
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger Task", 256, NULL, 1, NULL);
    xTaskCreate(echo_task, "Echo Task", 256, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED Task", 256, NULL, 1, NULL);

    vTaskStartScheduler();
    while (1);
}
