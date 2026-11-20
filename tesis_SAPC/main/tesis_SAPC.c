
/* TESIS */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_random.h"
#include "wifi_portal.h"

static const char *TAG = "tesis";

//====================================================================
// MODO SIMULACIÓN
//====================================================================
// 1 = no se leen los sensores: se usan valores del patrón de abajo + ruido
// 0 = lectura real del ADC (comportamiento original)
#define SIMULAR_SENSORES   1

// Amplitud del ruido simulado en cuentas ADC (±). 0 = sin ruido.
#define RUIDO_SIM          40
 
//====================================================================
// CONFIGURACIÓN MQTT
// (la red Wi-Fi y la IP/puerto del broker ya no van acá: se cargan desde
//  el portal de configuración y quedan guardadas en NVS, ver wifi_portal.c)
//====================================================================
#define MQTT_TOPIC     "esp32/vector_datos"

 
//====================================================================
// MATRIZ
//====================================================================
#define NUM_FILAS      4
#define NUM_COLS       8
#define NUM_SENSORES   (NUM_FILAS * NUM_COLS)      // 32
 
// Peor caso del mensaje: cada valor ocupa hasta 4 dígitos ("4095") + 1 coma
// -> 32 * 5 = 160 caracteres (la última coma no está, pero deja lugar al '\0').
// Para 12x8 (96 sensores) serían 480: el tamaño se ajusta solo.
#define PAYLOAD_MAX    (NUM_SENSORES * 5 + 1)
 
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;
 
//====================================================================
// CONFIGURACIÓN DE PINES DE LA ESP
//====================================================================
// Mux 1 (S0, S1, S2, S3): selecciona la FILA (0 a 3)
const gpio_num_t MUXFIL_PINS[4] = {GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_15, GPIO_NUM_7};
 
// Mux 2 (S0, S1, S2, S3): selecciona la COLUMNA (0 a 7)
const gpio_num_t MUXCOL_PINS[4] = {GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_18};
 
// Pin ADC en ESP32-S3: GPIO4 corresponde a ADC1, Canal 3
#define ADC_UNIT    ADC_UNIT_1
#define ADC_CHANNEL ADC_CHANNEL_3
 
adc_oneshot_unit_handle_t adc1_handle;
 
//====================================================================
// VARIABLES COMPARTIDAS ENTRE TAREAS
//====================================================================
// Último barrido completo (0 a 4095). Se escribe y se lee con el
// spinlock tomado, para que la tarea MQTT nunca envíe una mezcla de
// dos barridos distintos.
static uint16_t valores_crudos[NUM_SENSORES];
static portMUX_TYPE lock_valores = portMUX_INITIALIZER_UNLOCKED;
 
//====================================================================
// FUNCIONES MQTT
//====================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado al Broker MQTT");
            mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Desconectado del Broker MQTT");
            mqtt_connected = false;
            break;
        default:
            break;
    }
}
 
static void mqtt_app_start(void) {
    char broker_uri[96];
    if (!wifi_portal_broker_uri(broker_uri, sizeof(broker_uri))) {
        // No debería pasar: wifi_portal_conectar() no vuelve sin broker cargado
        ESP_LOGE(TAG, "No hay broker MQTT configurado");
        return;
    }
    ESP_LOGI(TAG, "Broker MQTT: %s", broker_uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,   // el cliente copia el texto
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}
 
//====================================================================
// FUNCIONES AUXILIARES (MUX Y ADC)
//====================================================================
static void configure_mux_gpios(void) {
    ESP_LOGI(TAG, "Configurando pines de los Multiplexores y adc1...");
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(MUXFIL_PINS[i]);
        gpio_set_direction(MUXFIL_PINS[i], GPIO_MODE_OUTPUT);
 
        gpio_reset_pin(MUXCOL_PINS[i]);
        gpio_set_direction(MUXCOL_PINS[i], GPIO_MODE_OUTPUT);
    }
 
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));
 
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));
}
 
#if SIMULAR_SENSORES
//--------------------------------------------------------------------
// Patrón simulado (cuentas ADC 0-4095). Reproduce el mapa dibujado:
//   - C0 y C7 sin presión (0)
//   - anillo exterior (C1-C6, F0-F3) presión baja  -> ~0.5 V (azul)
//   - centro (C2-C5, F1-F2) presión alta           -> ~2.5 V (amarillo)
// Conversión usada: V = cuentas * 3.3 / 4095  ->  0.5 V ≈ 620, 2.5 V ≈ 3100
// Editá esta tabla para probar otras formas de apoyo.
//--------------------------------------------------------------------
#define SIM_BAJO   620
#define SIM_ALTO   3100

static const uint16_t patron_sim[NUM_FILAS][NUM_COLS] = {
    /*        C0  C1        C2        C3        C4        C5        C6        C7 */
    /* F0 */ { 0, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, 0 },
    /* F1 */ { 0, SIM_BAJO, SIM_ALTO, SIM_ALTO, SIM_ALTO, SIM_ALTO, SIM_BAJO, 0 },
    /* F2 */ { 0, SIM_BAJO, SIM_ALTO, SIM_ALTO, SIM_ALTO, SIM_ALTO, SIM_BAJO, 0 },
    /* F3 */ { 0, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, SIM_BAJO, 0 },
};

// Devuelve el valor simulado de una celda, con ruido aleatorio ±RUIDO_SIM.
// Las celdas en 0 quedan exactamente en 0 (sin presión).
static int leer_sensor_simulado(uint8_t fila, uint8_t col) {
    int base = patron_sim[fila][col];
    if (base == 0 || RUIDO_SIM == 0) {
        return base;
    }
    int ruido = (int)(esp_random() % (2 * RUIDO_SIM + 1)) - RUIDO_SIM;
    int valor = base + ruido;
    if (valor < 0)    valor = 0;
    if (valor > 4095) valor = 4095;
    return valor;
}
#endif

static void set_mux_channel(int mux_id, uint8_t channel) {
    const gpio_num_t *pins = (mux_id == 1) ? MUXFIL_PINS : MUXCOL_PINS;
    for (int i = 0; i < 4; i++) {
        int bit_value = (channel >> i) & 0x01;
        gpio_set_level(pins[i], bit_value);
    }
}
 
//====================================================================
// TAREAS FREERTOS
//====================================================================
void Task_Adquisicion(void *pvParameters) {
    ESP_LOGI(TAG, "Tarea de Adquisición iniciada.");
#if SIMULAR_SENSORES
    ESP_LOGW(TAG, "MODO SIMULACIÓN ACTIVO: no se lee el ADC");
#endif
    int adc_raw = 0;
    uint16_t barrido[NUM_SENSORES];   // barrido en curso (local)
 
    while (1) {
        int indice_lineal = 0;
 
        // Bucle exterior: MUX 1 -> filas Y0 a Y3
        for (uint8_t ch_fila = 0; ch_fila < NUM_FILAS; ch_fila++) {
            set_mux_channel(1, ch_fila);
 
            // Bucle interior: MUX 2 -> columnas Y0 a Y7
            for (uint8_t ch_col = 0; ch_col < NUM_COLS; ch_col++) {
                set_mux_channel(2, ch_col);
 
                // Pausa de 1 ms para estabilización antes de medir (32 sensores x 1 ms = 32 ms por matriz)
                vTaskDelay(pdMS_TO_TICKS(1));
 
#if SIMULAR_SENSORES
                // Valor simulado (no hace falta tener la manta conectada)
                adc_raw = leer_sensor_simulado(ch_fila, ch_col);
#else
                // Lectura directa sin promedio
                ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL, &adc_raw));
#endif
                barrido[indice_lineal] = (uint16_t)adc_raw;
                indice_lineal++;
            }
        }
 
        // Publicar el barrido completo de una sola vez
        portENTER_CRITICAL(&lock_valores);
        memcpy(valores_crudos, barrido, sizeof(valores_crudos));
        portEXIT_CRITICAL(&lock_valores);
    }
}
 
void Task_MQTT(void *pvParameters) {
    char payload_buffer[PAYLOAD_MAX];
    uint16_t copia[NUM_SENSORES];
 
    while (1) {
        if (mqtt_connected) {
            // Copia del último barrido completo
            portENTER_CRITICAL(&lock_valores);
            memcpy(copia, valores_crudos, sizeof(copia));
            portEXIT_CRITICAL(&lock_valores);
 
            // Arma "v0,v1,...,v31" con un bucle: sirve para cualquier tamaño de matriz
            int n = 0;
            for (int i = 0; i < NUM_SENSORES && n < (int)sizeof(payload_buffer); i++) {
                n += snprintf(payload_buffer + n, sizeof(payload_buffer) - n,
                              (i == 0) ? "%u" : ",%u", (unsigned)copia[i]);
            }
 
            if (n >= (int)sizeof(payload_buffer)) {
                ESP_LOGE(TAG, "payload_buffer chico: el mensaje se truncaría, no se envía");
            } else {
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload_buffer, n, 0, 0);
                ESP_LOGI(TAG, "TX MQTT Crudo (%d bytes): [%s]", n, payload_buffer);
            }
        }
        // Envío cada 1 segundo (1000 ms)
        vTaskDelay(pdMS_TO_TICKS(1000)); //CAMBIAR POR 10000
    }
}
 
void app_main(void) {
    // Inicializar NVS para Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
 
    ESP_LOGI(TAG, "Tick rate: %d Hz", configTICK_RATE_HZ);
    configure_mux_gpios();

    // Bloquea hasta tener Wi-Fi y broker configurados: usa la configuración
    // guardada o abre el portal. Nada de lo que sigue arranca sin conexión.
    wifi_portal_conectar();

    mqtt_app_start();
 
    xTaskCreate(Task_Adquisicion, "Task_Adq", 4096, NULL, 5, NULL);
    xTaskCreate(Task_MQTT, "Task_MQTT", 4096, NULL, 4, NULL);
}