import queue
import paho.mqtt.client as mqtt
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.colors import LinearSegmentedColormap
import numpy as np

# ================= CONFIGURACIÓN MQTT =================
BROKER_HOST = "127.0.0.1"
BROKER_PORT = 1883
MQTT_TOPIC  = "esp32/vector_datos"
VECTOR_LEN  = 4  # 4 sensores (Matriz de 2x2)

cola_datos = queue.Queue()

# ================= CALLBACKS DE MQTT =================
def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"[MQTT] Conectado al Broker ({BROKER_HOST}:{BROKER_PORT})")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"[MQTT] Error de conexión: {reason_code}")

def on_message(client, userdata, msg):
    try:
        texto = msg.payload.decode('utf-8').strip()
        
        # 1. Leer los enteros crudos del ADC (0 a 4095)
        vector_crudo = [float(val) for val in texto.split(',')]
        
        # 2. Convertir de cuentas crudas (0-4095) a Voltios (0.00V - 3.30V)
        vector_voltios = [(val * 3.3) / 4095.0 for val in vector_crudo]
        
        if len(vector_voltios) == VECTOR_LEN:
            cola_datos.put(vector_voltios)
            print(f"Crudo: {vector_crudo} -> Voltios: {[round(v, 2) for v in vector_voltios]}")
    except Exception as e:
        print(f"[Error parseando trama]: {e}")

try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
except AttributeError:
    client = mqtt.Client()

client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
client.loop_start()

# ================= CONFIGURACIÓN DEL MAPA DE CALOR 2x2 =================
fig, ax = plt.subplots(figsize=(6, 5))

# Mapa de color personalizado: 0.0V = Blanco puro (#FFFFFF), 3.3V = Rojo intenso (#D90000)
cmap_blanco_rojo = LinearSegmentedColormap.from_list("BlancoRojo", ["#FFFFFF", "#FF8080", "#D90000"])

matriz_inicial = np.zeros((2, 2))
im = ax.imshow(matriz_inicial, cmap=cmap_blanco_rojo, vmin=0.0, vmax=3.3)

# Barra de escala lateral
cbar = fig.colorbar(im, ax=ax)
cbar.set_label("Tensión en GPIO4 (V)", fontweight='bold')

# Configuración de ejes y etiquetas de los sensores
ax.set_xticks([0, 1])
ax.set_yticks([0, 1])
ax.set_xticklabels(["Columna 0 (MUX2 Y0)", "Columna 1 (MUX2 Y1)"])
ax.set_yticklabels(["Fila 0 (MUX1 Y0)", "Fila 1 (MUX1 Y1)"])
ax.set_title("Mapa de Calor Matriz 2x2 (Tensión GPIO4)", fontsize=12, fontweight='bold')

# Crear las etiquetas de texto en el centro de cada cuadrante
textos_celdas = [[ax.text(j, i, "0.00 V", ha="center", va="center", 
                          color="black", fontsize=14, fontweight="bold") 
                  for j in range(2)] for i in range(2)]

# Líneas divisorias para distinguir las celdas cuando están en blanco (0V)
ax.set_xticks(np.arange(-0.5, 2, 1), minor=True)
ax.set_yticks(np.arange(-0.5, 2, 1), minor=True)
ax.grid(which="minor", color="gray", linestyle='-', linewidth=1.5)
ax.tick_params(which="minor", bottom=False, left=False)

def actualizar_mapa(frame):
    ultimo_vector = None
    while not cola_datos.empty():
        ultimo_vector = cola_datos.get_nowait()

    if ultimo_vector is not None:
        # Transformar el vector de 4 elementos en una matriz de 2 filas x 2 columnas
        matriz_2x2 = np.array(ultimo_vector).reshape((2, 2))
        im.set_data(matriz_2x2)

        # Actualizar el texto de cada celda con el valor en Voltios
        for i in range(2):
            for j in range(2):
                valor_v = matriz_2x2[i, j]
                textos_celdas[i][j].set_text(f"{valor_v:.2f} V")
                # Cambiar texto a blanco si el fondo rojo es muy oscuro (> 2.2V)
                textos_celdas[i][j].set_color("white" if valor_v > 2.2 else "black")

    return [im]

ani = FuncAnimation(fig, actualizar_mapa, interval=100, blit=False, cache_frame_data=False)

try:
    plt.tight_layout()
    plt.show()
except KeyboardInterrupt:
    print("Cerrando monitor...")
finally:
    client.loop_stop()
    client.disconnect()