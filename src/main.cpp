#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#include <hardware/adc.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/uart.h>

#include "hc06/hc06.h"
#include "pins/pins.h"
#include "mpu6050.h"

#include "edge-impulse-sdk/classifier/ei_model_types.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

using namespace ei;

extern "C" EI_IMPULSE_ERROR
run_classifier(ei::signal_t *signal, ei_impulse_result_t *result, bool debug);

const int MPU_ADDRESS = 0x68;
const int I2C_SDA_GPIO = 20;
const int I2C_SCL_GPIO = 21;

const uint BTN_R_PIN = 19;
const uint BTN_Y_PIN = 18;
const uint BTN_B_PIN = 17;
const uint BTN_G_PIN = 16;

#define LED_BT_PIN 12   // LED de status do Bluetooth (GP12)

static void send_bt_line(const char *line) {
    hc06_send_text(line);
}

static void mpu6050_init(){
    i2c_init(i2c_default, 400 * 1000);
    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);

    uint8_t buf[] = { 0x6B, 0x00 };
    i2c_write_blocking(i2c_default, MPU_ADDRESS, buf, 2, false);
}

static void mpu6050_read_accel(int16_t accel[3]){
    uint8_t buffer[6];
    uint8_t reg = 0x3B;

    i2c_write_blocking(i2c_default, MPU_ADDRESS, &reg, 1, true);
    i2c_read_blocking(i2c_default, MPU_ADDRESS, buffer, 6, false);

    for (int i = 0; i < 3; i++) {
        accel[i] = (buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
    }
}

static void input_joystick_task(void *p) {
    const uint button_pins[] = {BTN_R_PIN, BTN_Y_PIN, BTN_B_PIN, BTN_G_PIN, JOYSTICK_SW};
    const char *button_names[] = {"R", "Y", "B", "G", "SW"};
    bool last_state[] = {true, true, true, true, true};

    adc_init();
    adc_gpio_init(JOYSTICK_X);
    adc_gpio_init(JOYSTICK_Y);

    for (size_t i = 0; i < 5; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }

    int x_samples[5] = {0};
    int y_samples[5] = {0};
    int x_sum = 0;
    int y_sum = 0;
    int x_index = 0;
    int y_index = 0;

    while (true) {
        adc_select_input(0);
        int raw_x = adc_read();
        x_sum -= x_samples[x_index];
        x_samples[x_index] = raw_x;
        x_sum += x_samples[x_index];
        x_index = (x_index + 1) % 5;

        int x_filtered = x_sum / 5;
        int x_centered = x_filtered - 2047;
        int x_value = x_centered / 64;
        if (x_value > -2 && x_value < 2) {
            x_value = 0;
        }
        if (x_value != 0) {
            char frame[32];
            snprintf(frame, sizeof(frame), "JOY,X,%d\n", x_value);
            send_bt_line(frame);
        }

        adc_select_input(1);
        int raw_y = adc_read();
        y_sum -= y_samples[y_index];
        y_samples[y_index] = raw_y;
        y_sum += y_samples[y_index];
        y_index = (y_index + 1) % 5;

        int y_filtered = y_sum / 5;
        int y_centered = y_filtered - 2047;
        int y_value = -(y_centered / 64);
        if (y_value > -2 && y_value < 2) {
            y_value = 0;
        }
        if (y_value != 0) {
            char frame[32];
            snprintf(frame, sizeof(frame), "JOY,Y,%d\n", y_value);
            send_bt_line(frame);
        }

        for (size_t i = 0; i < 5; i++) {
            bool current_state = gpio_get(button_pins[i]);
            if (current_state != last_state[i]) {
                last_state[i] = current_state;
                char frame[32];
                snprintf(frame, sizeof(frame), "BTN,%s,%d\n", button_names[i], current_state ? 0 : 1);
                send_bt_line(frame);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void hc06_send_gesture(const char *label, float confidence) {
    char frame[96];
    snprintf(frame, sizeof(frame), "GESTURE,%s,%.3f\n", label, confidence);
    hc06_send_text(frame);
}

static void gesture_recognize_task(void *p){
    mpu6050_init();

    int16_t accelerometer[3] = { 0 };

    while (true) {
        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };

        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
            mpu6050_read_accel(accelerometer);
            buffer[ix + 0] = accelerometer[0];
            buffer[ix + 1] = accelerometer[1];
            buffer[ix + 2] = accelerometer[2];

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        ei::signal_t signal;
        numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

        ei_impulse_result_t result = { 0 };
        run_classifier(&signal, &result, false);

        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            ei_printf("    %s: %.5f\n",
                result.classification[ix].label,
                result.classification[ix].value);
        }

        size_t best_ix = 0;
        float best_value = result.classification[0].value;
        for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            if (result.classification[ix].value > best_value) {
                best_value = result.classification[ix].value;
                best_ix = ix;
            }
        }

        const char *label = result.classification[best_ix].label;
        ei_printf(">> Classe vencedora: %s (%.3f)\n", label, best_value);

        // Só transmite via BT se for um gesto real (não idle) com confiança suficiente.
        // O release é feito por timeout no Python — idle nunca precisa ser enviado.
        if (strcmp(label, "idle") != 0 && best_value >= 0.70f) {
            hc06_send_gesture(label, best_value);
        }
    }
}

static volatile uint32_t last_bt_rx_time = 0;

// ─── Task: Recebe Heartbeat do Python ────────────────────────────────────────
static void bt_rx_task(void *p) {
    while (true) {
        while (uart_is_readable(HC06_UART_ID)) {
            uart_getc(HC06_UART_ID);
            last_bt_rx_time = to_ms_since_boot(get_absolute_time());
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── Task: LED de status do Bluetooth ────────────────────────────────────────
// GP12 aceso    = BT conectado (recebendo heartbeat)
// GP12 piscando = BT desconectado (timeout de 3 segundos)
static void led_status_task(void *p) {
    gpio_init(LED_BT_PIN);
    gpio_set_dir(LED_BT_PIN, GPIO_OUT);
    gpio_put(LED_BT_PIN, 0);

    bool led_blink = false;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        // Se last_bt_rx_time for 0 (nunca recebeu) ou passou mais de 3s desde o último
        bool conectado = (last_bt_rx_time != 0) && ((now - last_bt_rx_time) < 3000);

        if (conectado) {
            gpio_put(LED_BT_PIN, 1);   // aceso fixo
        } else {
            led_blink = !led_blink;    // toggle a cada 500 ms → pisca 1 Hz
            gpio_put(LED_BT_PIN, led_blink ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int main(void){
    stdio_init_all();
    sleep_ms(2000);
    printf("[BOOT] stdio_init_all() done\n");
    hc06_init_uart();
    xTaskCreate(gesture_recognize_task, "gesture_task", 8192, NULL, 1, NULL);
    xTaskCreate(input_joystick_task,    "input_task",   2048, NULL, 1, NULL);
    xTaskCreate(led_status_task,        "led_bt",        256, NULL, 1, NULL);
    xTaskCreate(bt_rx_task,             "bt_rx",         256, NULL, 1, NULL);
    vTaskStartScheduler();

    while (true);
}