/* TESIS - Barrido 2x2 en 4ms (Sin promedio, datos crudos cada 1s) */
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

static const char *TAG = "tesis";

//====================================================================
// CONFIGURACIÓN DE RED Y MQTT
//====================================================================
#define WIFI_SSID      "fh_f9a5b0"
#define WIFI_PASS      "wlan065a4f"
#define BROKER_URI     "mqtt://192.168.1.2:1883"
#define MQTT_TOPIC     "esp32/vector_datos"
#define NUM_SENSORES   4

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

//====================================================================
// CONFIGURACIÓN DE PINES DE LA ESP
//====================================================================
// Mapeo de pines GPIO para Mux 1 (S0, S1, S2, S3)
const gpio_num_t MUXFIL_PINS[4] = {GPIO_NUM_17, GPIO_NUM_16, GPIO_NUM_15, GPIO_NUM_7};

// Mapeo de pines GPIO para Mux 2 (S0, S1, S2, S3)
const gpio_num_t MUXCOL_PINS[4] = {GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_18};

// Pin ADC en ESP32-S3: GPIO4 corresponde a ADC1, Canal 3
#define ADC_UNIT    ADC_UNIT_1
#define ADC_CHANNEL ADC_CHANNEL_3

adc_oneshot_unit_handle_t adc1_handle;

//====================================================================
// VARIABLES Y ESTRUCTURAS DE DATOS
//====================================================================
volatile uint16_t valores_crudos[NUM_SENSORES]; // Matriz temporal de 4 sensores (0 a 4095)

//====================================================================
// FUNCIONES WIFI Y MQTT
//====================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado, reconectando...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

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
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URI,
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
    int adc_raw = 0;

    while (1) {
        int indice_lineal = 0;

        // Bucle exterior: Controla el MUX 1 (Salidas Y0 a Y1)
        for (uint8_t ch_fila = 0; ch_fila < 2; ch_fila++) {
            set_mux_channel(1, ch_fila);

            // Bucle interior: Controla el MUX 2 (Salidas Y0 a Y1)
            for (uint8_t ch_col = 0; ch_col < 2; ch_col++) {
                set_mux_channel(2, ch_col);

                // Pausa de 1 ms para estabilización antes de medir (4 sensores x 1 ms = 4 ms por matriz)
                vTaskDelay(pdMS_TO_TICKS(1));

                // Lectura directa sin promedio
                ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL, &adc_raw));
                valores_crudos[indice_lineal] = (uint16_t)adc_raw;
                indice_lineal++;
            }
        }
    }
}

void Task_MQTT(void *pvParameters) {
    char payload_buffer[64];

    while (1) {
        if (mqtt_connected) {
            // Envía los 4 valores crudos (enteros de 0 a 4095) que caigan en este instante
            snprintf(payload_buffer, sizeof(payload_buffer), "%u,%u,%u,%u",
                     valores_crudos[0], valores_crudos[1],
                     valores_crudos[2], valores_crudos[3]);

            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload_buffer, 0, 0, 0);
            ESP_LOGI(TAG, "TX MQTT Crudo: [%s]", payload_buffer);
        }
        // Envío cada 1 segundo (1000 ms)
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    wifi_init_sta();
    mqtt_app_start();

    xTaskCreate(Task_Adquisicion, "Task_Adq", 4096, NULL, 5, NULL);
    xTaskCreate(Task_MQTT, "Task_MQTT", 4096, NULL, 4, NULL);
}