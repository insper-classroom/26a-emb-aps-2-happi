#include <FreeRTOS.h>
#include <queue.h>
#include <stdio.h>
#include <string.h>
#include <task.h>
#include <stdlib.h>
#include <semphr.h>

#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "hc06/hc06.h"
#include "pins/pins.h"

#define HC06_NAME "APS-ISAAC"
#define HC06_PIN "1234"

#define BTN_R_PIN 19
#define BTN_Y_PIN 18
#define BTN_B_PIN 17
#define BTN_G_PIN 16

#define BTN_AXIS_R 3
#define BTN_AXIS_Y 4
#define BTN_AXIS_B 5
#define BTN_AXIS_G 6

typedef struct {
    int eixo;
    int16_t valor;
} joystick_data;

QueueHandle_t xQueueRX;
QueueHandle_t xQueueTX;
QueueHandle_t xQueueADC;

void x_task(void *p){

    int ind = 0;
    joystick_data dados_env;
    int samples[5] = {0, 0, 0, 0, 0};
    int soma = 0;
    dados_env.eixo = 0;

    while(1){
        adc_select_input(0); 
        uint16_t raw_x = adc_read();
        soma = soma - samples[ind];
        samples[ind] = raw_x; //subst amostra mais antiga pela nova leitura
        soma += samples[ind];
        
        int filtrado_x = soma / 5; //nova media
        int centrado_x = filtrado_x - 2047;
        int x_escala_certa = centrado_x / 8 ; 
        
        if((x_escala_certa > -30) && (x_escala_certa < 30)){ //deadzone
            dados_env.valor = 0;            
        }else{
            dados_env.valor = x_escala_certa * 0.1;
        }
        
        if(dados_env.valor != 0){
            if(dados_env.valor != 0xFF){
                //envia o struct
                xQueueSend(xQueueADC,&dados_env,0);
            }
        }

        ind = (ind + 1)% 5; //força voltar pro começo quando chegar no final do array
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void y_task(void *p){
    int ind = 0;
    joystick_data dados_env;
    int samples[5] = {0, 0, 0, 0, 0};
    int soma = 0;
    dados_env.eixo = 1;

    while(1){
        adc_select_input(1);
        uint16_t raw_y = adc_read();  
        soma = soma - samples[ind];
        samples[ind] = raw_y;
        soma += samples[ind];
        
        int filtrado_y = soma / 5;
        int centrado_y = filtrado_y - 2047;
        int y_escala_certa = centrado_y/8;
        
        if((y_escala_certa > -30) && (y_escala_certa < 30)){
            dados_env.valor = 0;
        }else{
            dados_env.valor = -y_escala_certa * 0.1;
        }
    
       if(dados_env.valor != 0){
            if(dados_env.valor != 0xFF){
                xQueueSend(xQueueADC,&dados_env,0);
            }
        }

        ind = (ind + 1)% 5;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void click_task(void *p) {
    // Configura o pino como entrada com pull-up
    gpio_init(JOYSTICK_SW);
    gpio_set_dir(JOYSTICK_SW, GPIO_IN);
    gpio_pull_up(JOYSTICK_SW);

    joystick_data dados_env;
    dados_env.eixo = 2; // O ID "2" vai significar CLIQUE para o nosso Python depois
    
    bool estado_anterior = true; // true porque com pull-up, solto é 1

    while(1) {
        bool estado_atual = gpio_get(JOYSTICK_SW);
        
        // Só envia mensagem se o estado do botão MUDOU
        if(estado_atual != estado_anterior) {
            
            // Se estado_atual for false (0), foi pressionado. Vamos enviar valor 1.
            // Se estado_atual for true (1), foi solto. Vamos enviar valor 0.
            if (estado_atual == false) {
                dados_env.valor = 1; 
            } else {
                dados_env.valor = 0;
            }

            // Envia para a fila do ADC (a mesma fila que junta os dados de X e Y)
            xQueueSend(xQueueADC, &dados_env, 0);
            
            estado_anterior = estado_atual;
            vTaskDelay(pdMS_TO_TICKS(50)); // Aquele debounce maroto para não dar duplo-clique
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void joystick_bt_task(void *p){
    joystick_data data_receb;
    
    while(1){
        // Fica esperando chegar dados dos eixos X e Y
        if(xQueueReceive(xQueueADC, &data_receb, portMAX_DELAY)){
            
            // Só envia os dados do mouse se o Bluetooth estiver CONECTADO
            // (Para não encher a fila enquanto você ainda está pareando)
            // if (gpio_get(HC06_STATE_PIN) == 1) { 
                
                uint8_t sync = 0xFF;
                uint8_t eixo = data_receb.eixo;
                uint8_t lsb = data_receb.valor & 0xFF;
                uint8_t msb = (data_receb.valor >> 8) & 0xFF;

                // Envia os 4 bytes para a fila de transmissão do Bluetooth
                xQueueSend(xQueueTX, &sync, portMAX_DELAY);
                xQueueSend(xQueueTX, &eixo, portMAX_DELAY);
                xQueueSend(xQueueTX, &lsb, portMAX_DELAY);
                xQueueSend(xQueueTX, &msb, portMAX_DELAY);
            // }
        }
    }
}

void uart_rx_handler() {
        uint8_t ch = uart_getc(HC06_UART_ID);
        xQueueSendFromISR(xQueueRX, &ch, 0);
}

void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));

    int __unused actual = uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);

    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(HC06_UART_ID, false, false);

    // Set our data format
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

void init_uart_irq() {
    uart_set_fifo_enabled(HC06_UART_ID, false);

    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);

    uart_set_irq_enables(HC06_UART_ID, true, false);
}

static void tx_task(void* p) {
    uint8_t ch;
    while (true) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE) {
            uart_putc_raw(HC06_UART_ID, ch);
        }
    }
}

static void serial_task(void* p) {
    uint8_t ch;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            ch = (uint8_t)c;
            xQueueSend(xQueueTX, &ch, 0);
        }

        while (xQueueReceive(xQueueRX, &ch, 0) == pdTRUE) {
            // Heartbeat: não imprime no terminal.
            if (ch == 0x00) continue;
            putchar_raw(ch);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void buttons_bt_task(void *p) {
    const uint button_pins[] = {BTN_R_PIN, BTN_Y_PIN, BTN_B_PIN, BTN_G_PIN};
    const uint8_t button_axes[] = {BTN_AXIS_R, BTN_AXIS_Y, BTN_AXIS_B, BTN_AXIS_G};
    bool last_state[] = {true, true, true, true};

    for (int i = 0; i < 4; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }

    while (true) {
        for (int i = 0; i < 4; i++) {
            bool current_state = gpio_get(button_pins[i]);
            if (current_state != last_state[i]) {
                last_state[i] = current_state;

                joystick_data dados_env;
                dados_env.eixo = button_axes[i];
                dados_env.valor = current_state ? 0 : 1;
                xQueueSend(xQueueADC, &dados_env, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int main(void) {
    stdio_init_all();
    adc_init();

    adc_gpio_init(JOYSTICK_X);
    adc_gpio_init(JOYSTICK_Y);
    adc_gpio_init(JOYSTICK_SW);

    init_uart_hc06();
    init_uart_irq();

    xQueueRX = xQueueCreate(256, sizeof(uint8_t));
    xQueueTX = xQueueCreate(256, sizeof(uint8_t));
    xQueueADC = xQueueCreate(10, sizeof(joystick_data));

    xTaskCreate(tx_task, "TX", 512, NULL, 2, NULL);
    xTaskCreate(serial_task, "Serial", 1024, NULL, 1, NULL);
    xTaskCreate(buttons_bt_task, "Buttons", 512, NULL, 1, NULL);
    xTaskCreate(x_task, "Eixo_X", 1024, NULL, 1, NULL);
    xTaskCreate(y_task, "Eixo_Y", 1024, NULL, 1, NULL);
    xTaskCreate(click_task, "Clique", 1024, NULL, 1, NULL);
    xTaskCreate(joystick_bt_task, "Joy_BT", 1024, NULL, 1, NULL);
    
    vTaskStartScheduler();

    while (true);
}
