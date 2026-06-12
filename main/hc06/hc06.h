#ifndef HC06_H_
#define HC06_H_

#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

#define HC06_UART_ID uart1
#define HC06_BAUD_RATE 115200
#define HC06_RX_PIN 4
#define HC06_TX_PIN 5
#define HC06_ENABLE_PIN 15
#define HC06_STATE_PIN 13
// 1: STATE=1 quando conectado. 0: STATE=0 quando conectado.
#define HC06_STATE_ACTIVE_HIGH 0
// Pull interno do pino STATE (ajuda quando o pino fica flutuando).
// 0: pull-down | 1: pull-up
#define HC06_STATE_PULL_UP 1

// Número de amostras consecutivas para considerar o STATE estável.
// (com o loop a ~20ms, 5 amostras ≈ 100ms)
#define HC06_STATE_STABLE_SAMPLES 5

// Modo de detecção de conexão:
// 0: baseado em atividade na UART (recebeu algo recentemente)
// 1: baseado no pino STATE
#define HC06_CONN_MODE 1

// Se HC06_CONN_MODE=0, considera "conectado" se recebeu algo nos últimos X ms.
#define HC06_RX_ACTIVITY_CONNECTED_MS 3000

#ifdef __cplusplus
extern "C" {
#endif

void hc06_init_uart(void);
void hc06_send_text(const char *text);

bool hc06_check_connection();
bool hc06_set_name(char name[]);
bool hc06_set_pin(char pin[]);
bool hc06_set_baud_115200();
bool hc06_set_at_mode(int on);
bool hc06_config(char name[], char pin[]);

#ifdef __cplusplus
}
#endif


#endif // HC06_H_
