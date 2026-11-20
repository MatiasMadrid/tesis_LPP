#pragma once

#include <stdbool.h>
#include <stddef.h>

// Conecta la ESP a una red Wi-Fi y NO VUELVE hasta lograrlo.
//
//  1. Si hay configuración guardada en NVS (red + broker MQTT), intenta
//     conectarse a esa red.
//  2. Si no hay configuración completa (o no se pudo conectar), levanta la
//     red "SAPC-Config-XXXX" con un portal web para cargar SSID, contraseña
//     e IP/puerto del broker MQTT.
//  3. Al recibir datos correctos (la red tiene Internet y el broker acepta
//     conexiones) los guarda en NVS, cierra el portal y vuelve.
//
// Después de volver, si se corta el Wi-Fi reconecta solo.
// Requiere haber llamado antes a nvs_flash_init().
void wifi_portal_conectar(void);

// true si en este momento hay conexión con IP.
bool wifi_portal_conectado(void);

// Dirección del broker MQTT configurada, como "mqtt://192.168.1.5:1883".
// Llamar después de wifi_portal_conectar(). Devuelve false si no hay broker
// configurado o el buffer es chico.
bool wifi_portal_broker_uri(char *uri, size_t len);

// Borra la red y el broker guardados (la próxima vez que arranque abre el portal).
void wifi_portal_borrar_credenciales(void);
