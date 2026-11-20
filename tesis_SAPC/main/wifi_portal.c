//====================================================================
// wifi_portal.c
// Conexión Wi-Fi + dirección del broker MQTT con portal de
// configuración (captive portal).
//
// Flujo:
//   - Arranca en modo estación (STA) y prueba la configuración guardada
//     en NVS (red Wi-Fi + IP/puerto del broker MQTT).
//   - Si no hay configuración completa o no conecta, levanta un punto de
//     acceso (AP) "SAPC-Config-XXXX" + servidor DNS + servidor web.
//     Cualquier dirección que se escriba en el navegador lleva a la
//     página de configuración, y los celulares la abren solos.
//   - El usuario elige la red, escribe la contraseña y la IP del equipo
//     donde corre el broker. La ESP prueba la conexión Wi-Fi, que haya
//     Internet y que el broker responda; si todo funciona guarda los
//     datos en NVS, apaga el portal y sigue con el programa.
//   - Mantener presionado el botón BOOT 5 s borra toda la configuración
//     y reinicia (para cambiar de red o de broker).
//====================================================================
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dhcpserver/dhcpserver.h"
#include "wifi_portal.h"

static const char *TAG = "wifi_portal";

//====================================================================
// CONFIGURACIÓN
//====================================================================
// Red que levanta la ESP para configurarla. Se le agregan los últimos
// 4 dígitos de la MAC: "SAPC-Config-A1B2".
#define AP_SSID_PREFIJO        "SAPC-Config"
// Contraseña de la red de configuración. "" = red abierta (sin clave).
// Si se pone, mínimo 8 caracteres. Con clave, el SSID y la contraseña
// que se cargan en la página viajan cifrados por WPA2.
#define AP_PASSWORD            "sapc1234"
#define AP_CANAL               1
#define AP_MAX_CLIENTES        4

#define NVS_NAMESPACE          "wifi_cfg"

#define INTENTOS_POR_CONEXION  3        // reintentos internos en cada prueba
#define TIMEOUT_CONEXION_MS    15000    // tiempo máximo de cada prueba
// Con el portal abierto, cada cuánto se vuelve a probar la red guardada
// (por ejemplo si se cortó la luz y el router tarda en volver).
// Solo se prueba si no hay nadie conectado al portal.
#define REINTENTO_GUARDADA_MS  60000

// Al cargar una red NUEVA se verifica que tenga salida a Internet
// resolviendo este nombre. 0 = no verificar.
#define VERIFICAR_INTERNET     1
#define HOST_PRUEBA_INTERNET   "www.google.com"

// Broker MQTT
#define PUERTO_MQTT_DEFECTO    1883
// Al cargar una configuración NUEVA se verifica que el broker acepte
// conexiones TCP en IP:puerto. 0 = no verificar (se guarda igual).
#define VERIFICAR_BROKER       1
#define TIMEOUT_BROKER_MS      4000
#define LARGO_BROKER           64       // máx. 63 caracteres (IP o nombre)

// Botón para borrar la red guardada (GPIO0 = botón BOOT de la placa).
// -1 = desactivado.
#define BOTON_RESET_WIFI       0
#define TIEMPO_BOTON_MS        5000

//====================================================================
// ESTADO INTERNO
//====================================================================
#define BIT_CONECTADO    BIT0
#define BIT_FALLO        BIT1
#define BIT_NUEVAS_CRED  BIT2

typedef enum { MODO_INACTIVO, MODO_INTENTANDO, MODO_CONECTADO } modo_sta_t;
typedef enum { EST_ESPERANDO, EST_PROBANDO, EST_OK, EST_ERROR } estado_portal_t;

static EventGroupHandle_t s_eventos;
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;

static volatile modo_sta_t s_modo = MODO_INACTIVO;
static volatile int s_reintentos = 0;
static volatile uint8_t s_ultimo_motivo = 0;
static volatile int s_clientes_ap = 0;

// Datos que llegan desde la página y estado que la página consulta
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid_nuevo[33];
static char s_pass_nuevo[65];
static char s_broker_nuevo[LARGO_BROKER];
static uint16_t s_puerto_nuevo;
static volatile estado_portal_t s_estado_portal = EST_ESPERANDO;
static char s_msj_estado[300];

// Broker en uso (leído de NVS o recién cargado desde el portal)
static char s_broker[LARGO_BROKER];
static uint16_t s_puerto = PUERTO_MQTT_DEFECTO;

static httpd_handle_t s_httpd = NULL;
static volatile bool s_dns_activo = false;
static char s_url_portal[32];   // "http://192.168.4.1/"

// Página web (portal.html, embebida desde CMakeLists con EMBED_TXTFILES)
extern const char portal_html_start[] asm("_binary_portal_html_start");

//====================================================================
// UTILIDADES
//====================================================================
static void set_estado(estado_portal_t e, const char *msj) {
    taskENTER_CRITICAL(&s_lock);
    s_estado_portal = e;
    strlcpy(s_msj_estado, msj, sizeof(s_msj_estado));
    taskEXIT_CRITICAL(&s_lock);
}

// Decodifica "%XX" y '+' de un formulario (in-place)
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

// Copia src en dst escapando lo necesario para meterlo en un string JSON
static void json_escape(char *dst, size_t len, const char *src) {
    size_t j = 0;
    for (; *src && j + 7 < len; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c < 0x20) {
            j += snprintf(dst + j, len - j, "\\u%04x", c);
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = 0;
}

static const char *texto_motivo(uint8_t m) {
    switch (m) {
        case WIFI_REASON_NO_AP_FOUND:
            return "No se encontró la red. Revisá el nombre y que esté al alcance.";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "Contraseña incorrecta.";
        case 0:
            return "Tiempo de espera agotado (la red no respondió o no entregó IP).";
        default:
            return "No se pudo conectar a la red.";
    }
}

//====================================================================
// NVS: guardar / leer / borrar credenciales
//====================================================================
static bool leer_credenciales(char *ssid, size_t ls, char *pass, size_t lp) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ls) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &lp) == ESP_OK &&
              ssid[0] != 0;
    nvs_close(h);
    return ok;
}

static bool leer_broker(char *host, size_t lh, uint16_t *puerto) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "broker", host, &lh) == ESP_OK && host[0] != 0;
    if (ok && (nvs_get_u16(h, "puerto", puerto) != ESP_OK || *puerto == 0)) {
        *puerto = PUERTO_MQTT_DEFECTO;
    }
    nvs_close(h);
    return ok;
}

static void guardar_credenciales(const char *ssid, const char *pass,
                                 const char *broker, uint16_t puerto) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS para guardar la configuración");
        return;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_set_str(h, "broker", broker);
    nvs_set_u16(h, "puerto", puerto);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Red \"%s\" y broker %s:%u guardados en memoria", ssid, broker, puerto);
}

bool wifi_portal_broker_uri(char *uri, size_t len) {
    if (s_broker[0] == 0) return false;
    int n = snprintf(uri, len, "mqtt://%s:%u", s_broker, s_puerto);
    return n > 0 && n < (int)len;
}

void wifi_portal_borrar_credenciales(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "Configuración guardada borrada (red Wi-Fi y broker)");
}

bool wifi_portal_conectado(void) {
    return (xEventGroupGetBits(s_eventos) & BIT_CONECTADO) != 0;
}

//====================================================================
// EVENTOS WI-FI
//====================================================================
static void manejador_eventos(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_eventos, BIT_CONECTADO);

        if (s_modo == MODO_INTENTANDO) {
            // La desconexión que pedimos nosotros antes de probar no cuenta
            if (ev->reason == WIFI_REASON_ASSOC_LEAVE) return;
            s_ultimo_motivo = ev->reason;
            ESP_LOGW(TAG, "Intento fallido (motivo %d)", ev->reason);
            if (++s_reintentos < INTENTOS_POR_CONEXION) {
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_eventos, BIT_FALLO);
            }
        } else if (s_modo == MODO_CONECTADO) {
            ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %d), reconectando...", ev->reason);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi conectado! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_eventos, BIT_CONECTADO);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_clientes_ap++;
        ESP_LOGI(TAG, "Dispositivo conectado al portal (%d)", s_clientes_ap);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clientes_ap > 0) s_clientes_ap--;
        ESP_LOGI(TAG, "Dispositivo desconectado del portal (%d)", s_clientes_ap);
    }
}

// Prueba conectarse a una red. Bloquea hasta lograrlo o fallar.
static bool intentar_conexion(const char *ssid, const char *pass) {
    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, pass, strnlen(pass, sizeof(cfg.sta.password)));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    s_modo = MODO_INACTIVO;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    xEventGroupClearBits(s_eventos, BIT_CONECTADO | BIT_FALLO);
    s_reintentos = 0;
    s_ultimo_motivo = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_modo = MODO_INTENTANDO;
    ESP_LOGI(TAG, "Intentando conectar a \"%s\"...", ssid);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_eventos, BIT_CONECTADO | BIT_FALLO,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(TIMEOUT_CONEXION_MS));
    if (bits & BIT_CONECTADO) {
        s_modo = MODO_CONECTADO;
        return true;
    }
    s_modo = MODO_INACTIVO;
    esp_wifi_disconnect();
    return false;
}

static void desconectar(void) {
    s_modo = MODO_INACTIVO;
    esp_wifi_disconnect();
}

static bool hay_internet(void) {
#if VERIFICAR_INTERNET
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    for (int i = 0; i < 3; i++) {
        struct addrinfo *res = NULL;
        if (getaddrinfo(HOST_PRUEBA_INTERNET, "80", &hints, &res) == 0 && res != NULL) {
            freeaddrinfo(res);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
#else
    return true;
#endif
}

//====================================================================
// BROKER MQTT: validar la dirección y probar que responda
//====================================================================
// Acepta lo que el usuario pueda escribir o pegar:
//   "192.168.1.5"  "mqtt://192.168.1.5:1883/"  "mi-pc.local"
// Deja en host solo la IP/nombre; si venía ":puerto" pisa *puerto.
static bool normalizar_broker(char *host, int *puerto) {
    char *p = host;
    while (*p == ' ') p++;
    char *esquema = strstr(p, "://");
    if (esquema) p = esquema + 3;
    memmove(host, p, strlen(p) + 1);
    host[strcspn(host, "/ ")] = 0;

    char *dos_puntos = strchr(host, ':');
    if (dos_puntos) {
        *dos_puntos = 0;
        *puerto = atoi(dos_puntos + 1);
    }

    size_t n = strlen(host);
    if (n == 0 || n >= LARGO_BROKER) return false;
    bool solo_numeros = true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)host[i];
        if (!isalnum(c) && c != '.' && c != '-') return false;
        if (!isdigit(c) && c != '.') solo_numeros = false;
    }
    if (solo_numeros) {                    // si parece IP, que sea una IPv4 válida
        struct in_addr a;
        if (inet_pton(AF_INET, host, &a) != 1) return false;
    }
    return true;
}

// Abre (y cierra) una conexión TCP a host:puerto. Si falla deja el motivo en err.
static bool probar_broker(const char *host, uint16_t puerto, char *err, size_t lerr) {
#if VERIFICAR_BROKER
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char sp[8];
    snprintf(sp, sizeof(sp), "%u", puerto);
    if (getaddrinfo(host, sp, &hints, &res) != 0 || res == NULL) {
        snprintf(err, lerr, "No se encontró el equipo \"%s\". Revisá la dirección del broker.", host);
        return false;
    }

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        freeaddrinfo(res);
        snprintf(err, lerr, "Error interno al probar el broker.");
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    int error = (r == 0) ? 0 : errno;
    if (r != 0 && errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv = { .tv_sec = TIMEOUT_BROKER_MS / 1000,
                              .tv_usec = (TIMEOUT_BROKER_MS % 1000) * 1000 };
        if (select(s + 1, NULL, &wf, NULL, &tv) > 0) {
            socklen_t l = sizeof(error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &l);
        } else {
            error = ETIMEDOUT;
        }
    }
    close(s);

    if (error == 0) {
        ESP_LOGI(TAG, "Broker %s:%u responde", host, puerto);
        return true;
    }
    ESP_LOGW(TAG, "Broker %s:%u no responde (errno %d)", host, puerto, error);
    if (error == ECONNREFUSED || error == ECONNRESET) {
        snprintf(err, lerr, "El equipo %s responde, pero no hay broker en el puerto %u. "
                 "Revisá que Mosquitto esté corriendo y acepte conexiones de la red.", host, puerto);
    } else {
        snprintf(err, lerr, "No se pudo llegar al broker %s:%u. Revisá la IP, que el equipo "
                 "esté en la misma red y que el firewall permita el puerto %u.", host, puerto, puerto);
    }
    return false;
#else
    return true;
#endif
}

//====================================================================
// SERVIDOR DNS: responde cualquier nombre con la IP de la ESP
// (así cualquier dominio lleva al portal y los celulares lo detectan)
//====================================================================
static void tarea_dns(void *arg) {
    uint32_t ip_ap = (uint32_t)(uintptr_t)arg;   // ya en orden de red

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: no se pudo crear el socket");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in dir = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&dir, sizeof(dir)) < 0) {
        ESP_LOGE(TAG, "DNS: no se pudo usar el puerto 53");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[600];
    while (s_dns_activo) {
        struct sockaddr_in origen;
        socklen_t lo = sizeof(origen);
        int len = recvfrom(sock, buf, 512, 0, (struct sockaddr *)&origen, &lo);
        if (len < 12) continue;                         // timeout o basura
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;  // solo consultas de 1 pregunta

        // Saltar el nombre consultado (QNAME)
        int p = 12;
        while (p < len && buf[p] != 0) p += buf[p] + 1;
        p += 1;
        if (p + 4 > len) continue;
        uint16_t qtype = (uint16_t)((buf[p] << 8) | buf[p + 1]);
        p += 4;                                         // QTYPE + QCLASS

        // Armar la respuesta sobre el mismo buffer
        buf[2] |= 0x80;                                 // QR = respuesta
        buf[3] = 0x80;                                  // RA = 1, sin error
        buf[6] = 0;
        buf[7] = (qtype == 1) ? 1 : 0;                  // ANCOUNT: solo respondemos tipo A
        buf[8] = buf[9] = buf[10] = buf[11] = 0;

        int r = p;
        if (qtype == 1) {
            static const uint8_t resp[] = {
                0xC0, 0x0C,       // puntero al nombre de la pregunta
                0x00, 0x01,       // tipo A
                0x00, 0x01,       // clase IN
                0x00, 0x00, 0x00, 0x3C,   // TTL 60 s
                0x00, 0x04,       // 4 bytes de datos
            };
            memcpy(buf + r, resp, sizeof(resp));
            r += sizeof(resp);
            memcpy(buf + r, &ip_ap, 4);
            r += 4;
        }
        sendto(sock, buf, r, 0, (struct sockaddr *)&origen, lo);
    }
    close(sock);
    vTaskDelete(NULL);
}

//====================================================================
// SERVIDOR WEB
//====================================================================
static esp_err_t h_raiz(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_html_start, HTTPD_RESP_USE_STRLEN);
}

// Lista de redes visibles: [{"s":"ssid","r":-60,"a":1}, ...]
static esp_err_t h_scan(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    wifi_scan_config_t sc = { .show_hidden = false };
    if (s_modo == MODO_INTENTANDO || esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = 20;
    wifi_ap_record_t *lista = calloc(n, sizeof(wifi_ap_record_t));
    if (lista == NULL) {
        esp_wifi_clear_ap_list();
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&n, lista);

    httpd_resp_sendstr_chunk(req, "[");
    bool primero = true;
    for (int i = 0; i < n; i++) {
        if (lista[i].ssid[0] == 0) continue;
        bool repetida = false;                          // misma red con varios AP
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)lista[i].ssid, (char *)lista[j].ssid) == 0) { repetida = true; break; }
        }
        if (repetida) continue;

        char esc[200], item[260];
        json_escape(esc, sizeof(esc), (char *)lista[i].ssid);
        snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                 primero ? "" : ",", esc, lista[i].rssi,
                 lista[i].authmode != WIFI_AUTH_OPEN);
        httpd_resp_sendstr_chunk(req, item);
        primero = false;
    }
    free(lista);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t responder_json(httpd_req_t *req, bool ok, const char *msj) {
    char esc[200], out[260];
    json_escape(esc, sizeof(esc), msj);
    snprintf(out, sizeof(out), "{\"ok\":%s,\"m\":\"%s\"}", ok ? "true" : "false", esc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// Recibe ssid, pass, broker y puerto (application/x-www-form-urlencoded)
static esp_err_t h_guardar(httpd_req_t *req) {
    char cuerpo[768];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(cuerpo)) {
        return responder_json(req, false, "Datos inválidos.");
    }
    int leido = 0;
    while (leido < total) {
        int r = httpd_req_recv(req, cuerpo + leido, total - leido);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        leido += r;
    }
    cuerpo[leido] = 0;

    char ssid[100] = "", pass[200] = "";
    if (httpd_query_key_value(cuerpo, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return responder_json(req, false, "Falta el nombre de la red.");
    }
    if (httpd_query_key_value(cuerpo, "pass", pass, sizeof(pass)) != ESP_OK) {
        pass[0] = 0;                                    // red abierta
    }
    char broker[200] = "", puerto_txt[16] = "";
    if (httpd_query_key_value(cuerpo, "broker", broker, sizeof(broker)) != ESP_OK) {
        return responder_json(req, false, "Falta la IP del broker MQTT.");
    }
    httpd_query_key_value(cuerpo, "puerto", puerto_txt, sizeof(puerto_txt));
    url_decode(ssid);
    url_decode(pass);
    url_decode(broker);
    url_decode(puerto_txt);

    int puerto = puerto_txt[0] ? atoi(puerto_txt) : PUERTO_MQTT_DEFECTO;
    if (!normalizar_broker(broker, &puerto)) {
        return responder_json(req, false, "La IP del broker no es válida (ejemplo: 192.168.1.5).");
    }
    if (puerto < 1 || puerto > 65535) {
        return responder_json(req, false, "El puerto del broker debe estar entre 1 y 65535 (normalmente 1883).");
    }

    size_t ls = strlen(ssid), lp = strlen(pass);
    if (ls == 0 || ls > 32) {
        return responder_json(req, false, "El nombre de la red debe tener entre 1 y 32 caracteres.");
    }
    if (lp != 0 && (lp < 8 || lp > 64)) {
        return responder_json(req, false, "La contraseña debe tener entre 8 y 64 caracteres.");
    }
    if (s_estado_portal == EST_PROBANDO) {
        return responder_json(req, false, "Ya se está probando una red, esperá un momento.");
    }

    char msj[160];
    snprintf(msj, sizeof(msj), "Probando conexión a \"%s\"...", ssid);
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_ssid_nuevo, ssid, sizeof(s_ssid_nuevo));
    strlcpy(s_pass_nuevo, pass, sizeof(s_pass_nuevo));
    strlcpy(s_broker_nuevo, broker, sizeof(s_broker_nuevo));
    s_puerto_nuevo = (uint16_t)puerto;
    taskEXIT_CRITICAL(&s_lock);
    set_estado(EST_PROBANDO, msj);
    xEventGroupSetBits(s_eventos, BIT_NUEVAS_CRED);

    return responder_json(req, true, msj);
}

// Estado de la prueba: {"e":"probando|ok|error|esperando","m":"..."}
static esp_err_t h_estado(httpd_req_t *req) {
    static const char *nombres[] = { "esperando", "probando", "ok", "error" };
    char msj[sizeof(s_msj_estado)];
    estado_portal_t e;
    taskENTER_CRITICAL(&s_lock);
    e = s_estado_portal;
    strlcpy(msj, s_msj_estado, sizeof(msj));
    taskEXIT_CRITICAL(&s_lock);

    char esc[640], out[720];
    json_escape(esc, sizeof(esc), msj);
    snprintf(out, sizeof(out), "{\"e\":\"%s\",\"m\":\"%s\"}", nombres[e], esc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// Configuración guardada, para precargar el formulario (sin la contraseña):
// {"s":"ssid","b":"192.168.1.5","p":1883}
static esp_err_t h_config(httpd_req_t *req) {
    char ssid[33] = { 0 }, pass[65] = { 0 }, host[LARGO_BROKER] = { 0 };
    uint16_t puerto = PUERTO_MQTT_DEFECTO;
    if (!leer_credenciales(ssid, sizeof(ssid), pass, sizeof(pass))) ssid[0] = 0;
    if (!leer_broker(host, sizeof(host), &puerto)) host[0] = 0;
    memset(pass, 0, sizeof(pass));

    char es[200], eb[200], out[480];
    json_escape(es, sizeof(es), ssid);
    json_escape(eb, sizeof(eb), host);
    snprintf(out, sizeof(out), "{\"s\":\"%s\",\"b\":\"%s\",\"p\":%u}", es, eb, puerto);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// Cualquier otra dirección (ej. las que usan Android/iOS/Windows para
// detectar portales) se redirige a la página principal.
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_url_portal);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
}

static void iniciar_servidor_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;     // los celulares abren muchas conexiones
    cfg.stack_size = 6144;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor web");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = h_raiz },
        { .uri = "/scan",    .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/estado",  .method = HTTP_GET,  .handler = h_estado },
        { .uri = "/config",  .method = HTTP_GET,  .handler = h_config },
        { .uri = "/guardar", .method = HTTP_POST, .handler = h_guardar },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_404);
}

//====================================================================
// PORTAL: encender / apagar
//====================================================================
static void iniciar_portal(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-%02X%02X",
                     AP_SSID_PREFIJO, mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = AP_CANAL;
    ap.ap.max_connection = AP_MAX_CLIENTES;
    if (strlen(AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // El servidor DHCP del AP le indica a los clientes que usen a la ESP como DNS
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_netif_ap, &ip);
    snprintf(s_url_portal, sizeof(s_url_portal), "http://" IPSTR "/", IP2STR(&ip.ip));

    esp_netif_dhcps_stop(s_netif_ap);
    dhcps_offer_t ofrecer_dns = OFFER_DNS;
    esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &ofrecer_dns, sizeof(ofrecer_dns));
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_netif_ap);

    s_dns_activo = true;
    xTaskCreate(tarea_dns, "dns_portal", 3072, (void *)(uintptr_t)ip.ip.addr, 5, NULL);
    iniciar_servidor_web();
    set_estado(EST_ESPERANDO, "");

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, " PORTAL DE CONFIGURACIÓN ACTIVO");
    ESP_LOGW(TAG, " Red: %s   Clave: %s", (char *)ap.ap.ssid,
             strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : "(abierta)");
    ESP_LOGW(TAG, " Página: %s (o cualquier dirección)", s_url_portal);
    ESP_LOGW(TAG, "==================================================");
}

static void detener_portal(void) {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_dns_activo = false;              // la tarea DNS termina sola en ≤1 s
    esp_wifi_set_mode(WIFI_MODE_STA);  // apaga el AP, la conexión STA sigue
    ESP_LOGI(TAG, "Portal cerrado");
}

//====================================================================
// BOTÓN PARA BORRAR LA RED GUARDADA
//====================================================================
#if BOTON_RESET_WIFI >= 0
static void tarea_boton(void *arg) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOTON_RESET_WIFI,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int ms = 0;
    while (1) {
        if (gpio_get_level(BOTON_RESET_WIFI) == 0) {
            ms += 100;
            if (ms >= TIEMPO_BOTON_MS) {
                ESP_LOGW(TAG, "Botón mantenido %d s: se borra la red y se reinicia", TIEMPO_BOTON_MS / 1000);
                wifi_portal_borrar_credenciales();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

//====================================================================
// FUNCIÓN PRINCIPAL
//====================================================================
void wifi_portal_conectar(void) {
    s_eventos = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Las credenciales las guardamos nosotros en NVS; el driver no.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &manejador_eventos, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &manejador_eventos, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

#if BOTON_RESET_WIFI >= 0
    xTaskCreate(tarea_boton, "boton_wifi", 2048, NULL, 3, NULL);
#endif

    // 1) Configuración guardada (red Wi-Fi + broker). Hacen falta las dos.
    //    Con la configuración guardada NO se exige que el broker responda:
    //    el cliente MQTT reintenta solo (la PC puede encenderse después).
    char ssid_g[33] = { 0 }, pass_g[65] = { 0 };
    bool hay_wifi = leer_credenciales(ssid_g, sizeof(ssid_g), pass_g, sizeof(pass_g));
    bool hay_broker = leer_broker(s_broker, sizeof(s_broker), &s_puerto);
    bool hay_guardada = hay_wifi && hay_broker;
    if (hay_guardada) {
        ESP_LOGI(TAG, "Red guardada: \"%s\"  Broker: %s:%u", ssid_g, s_broker, s_puerto);
        for (int i = 0; i < 2; i++) {
            if (intentar_conexion(ssid_g, pass_g)) return;
        }
        ESP_LOGW(TAG, "No se pudo conectar a la red guardada");
    } else if (hay_wifi) {
        ESP_LOGW(TAG, "Hay red guardada pero falta la IP del broker MQTT");
    } else {
        ESP_LOGW(TAG, "No hay configuración guardada");
    }

    // 2) Portal de configuración (se queda acá hasta conectar)
    iniciar_portal();
    TickType_t ultimo_reintento = xTaskGetTickCount();

    while (1) {
        EventBits_t b = xEventGroupWaitBits(s_eventos, BIT_NUEVAS_CRED, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(1000));
        if (b & BIT_NUEVAS_CRED) {
            char ssid[33], pass[65], broker[LARGO_BROKER], msj[sizeof(s_msj_estado)];
            uint16_t puerto;
            taskENTER_CRITICAL(&s_lock);
            strlcpy(ssid, s_ssid_nuevo, sizeof(ssid));
            strlcpy(pass, s_pass_nuevo, sizeof(pass));
            strlcpy(broker, s_broker_nuevo, sizeof(broker));
            puerto = s_puerto_nuevo;
            taskEXIT_CRITICAL(&s_lock);

            if (intentar_conexion(ssid, pass)) {
                if (!hay_internet()) {
                    desconectar();
                    snprintf(msj, sizeof(msj), "Se conectó a \"%s\" pero la red no tiene acceso a Internet.", ssid);
                    set_estado(EST_ERROR, msj);
                } else {
                    snprintf(msj, sizeof(msj), "Wi-Fi OK. Probando el broker %s:%u...", broker, puerto);
                    set_estado(EST_PROBANDO, msj);
                    if (probar_broker(broker, puerto, msj, sizeof(msj))) {
                        guardar_credenciales(ssid, pass, broker, puerto);
                        strlcpy(s_broker, broker, sizeof(s_broker));
                        s_puerto = puerto;
                        esp_netif_ip_info_t ip;
                        esp_netif_get_ip_info(s_netif_sta, &ip);
                        snprintf(msj, sizeof(msj),
                                 "Conectado a \"%s\" (IP " IPSTR ") y broker %s:%u OK. "
                                 "Datos guardados. Ya podés cerrar esta página.",
                                 ssid, IP2STR(&ip.ip), broker, puerto);
                        set_estado(EST_OK, msj);
                        vTaskDelay(pdMS_TO_TICKS(5000));   // tiempo para que la página lo muestre
                        detener_portal();
                        return;
                    }
                    desconectar();
                    set_estado(EST_ERROR, msj);           // msj trae el motivo del broker
                }
            } else {
                set_estado(EST_ERROR, texto_motivo(s_ultimo_motivo));
            }
            ultimo_reintento = xTaskGetTickCount();

        } else if (hay_guardada && s_clientes_ap == 0 &&
                   s_estado_portal != EST_PROBANDO &&
                   (xTaskGetTickCount() - ultimo_reintento) > pdMS_TO_TICKS(REINTENTO_GUARDADA_MS)) {
            // Nadie usando el portal: reintentar la red guardada
            if (intentar_conexion(ssid_g, pass_g)) {
                detener_portal();
                return;
            }
            ultimo_reintento = xTaskGetTickCount();
        }
    }
}
