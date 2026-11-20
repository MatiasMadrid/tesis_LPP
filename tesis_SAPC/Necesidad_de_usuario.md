Usuario 1: Enfermero/Cuidador

| URD (Necesidad del usuario) | PRD (Requisito técnico) |
| ----- | ----- |
| Detectar zonas de riesgo sin inspección manual constante | Frecuencia de muestreo del sistema 0,1 Hz (actualización cada 10 s). \[1\],\[2\] |
|  | Frecuencia de muestreo de los sensores 1000 Hz |
|  | Resolución espacial de aproximadamente 50 mm. Tres zonas de 8x4, 32 sensores cada una. (60 cm x 30 cm) \[3\],\[4\] |
| Contar con información cuantitativa que permita hacer un adecuado seguimiento de los protocolos | Identificación de situaciones de potencial riesgo de formación de las lesiones. (hora, cuánto tiempo) |
|  | Poder registrar información en una base de datos (como InfluxDB, MongoDB u otra), siendo este paso necesario para la predicción. Registro temporal de la jornada de la actividad del personal para posibles informes.(para evaluar y optimizar el trabajo de los enfermeros) |
| Simplificar la aplicación del protocolo de rotación en situaciones de alta demanda de pacientes. | El sistema debe generar alertas automáticas cuando se superen presiones en determinado tiempo. Los umbrales son valores de proporción, por ejemplo 3/5 del mínimo valor de un vector de máximo por fila o columna (por área de sensado), por un determinado tiempo (1:30 hs \- 2:00 hs) |
|  | El sistema debe emitir alarmas visuales (en el módulo y en la interfaz) y sonoras (solo en la interfaz) diferenciadas según nivel de riesgo |
|  | El sistema debe permitir monitoreo continuo de los puntos de presión sin intervención manual |
| Acceder a información resumida y clara: datos de identificación del paciente, mapa de presión, alertas de presión, tiempo de recomendación de rotación, etc.  | La interfaz debe mostrar mapas de presión simplificados y estados de riesgo (normal / alerta / crítico). Barra de tiempo para recomendación de rotación (recomendación con horario) |
|  | Historial de posiciones del paciente (otro menú).  Listado de zonas críticas (zonas de riesgo) por semana o día. Incl uyendo el promedio de tiempo en cada zona. |
|  | Debe informar el tiempo medio de rotación acumulado a tantos días. (promedio de rotación). |
|  | Contemplar cambios de presión asociados a otras rutinas (ej: baño del paciente) |
|  | El sistema debe transmitir datos vía Wi-Fi para ser visualizados en una interfaz. |
| Usar el sistema sin capacitación compleja | El cubrecolchón debe ser de fácil colocación sobre el colchón (contemplando posibles correas de fijación). |
|  | El uso del sistema desde el módulo físico debe ser intuitivo (encendido y configuración). El módulo ubicado en la cama del paciente esta compuesto por un único botón de encendido y dos indicadores visuales.El botón permitirá activar o apagar el sistema, mientras que un LED RGB informará el estado general de funcionamiento mediante distintos colores, por ejemplo: rojo para indicar que el equipo está encendido, amarillo intermitente durante el proceso de conexión, y azul cuando la conexión se haya establecido correctamente.Además, el sistema podría incorporar un segundo indicador luminoso destinado a señalar el nivel de criticidad asociado a la necesidad de cambio de posición del paciente, proporcionando una alerta visual rápida sobre el riesgo detectado. |
|  | La conexión a una red WiFi la realiza el técnico en la instalación pero con la facilidad que también la pueda realizar usuario con ayuda de una guía, también en casos de cambios de la red wifi. |

Usuario 2: Paciente

| URD (Necesidad del usuario) | PRD (Requisito técnico) |
| ----- | ----- |
| Evitar la aparición de lesiones por presión | El sistema debe detectar zonas de presión elevada cuando se supere un umbral de presión por determinado tiempo, por ejemplo ⅓ del máximo valor sensado por matriz, por un determinado tiempo (1:30 hs \- 2:00 hs) |
| Permanecer cómodo durante el uso | El dispositivo debe ser flexible, delgado y no generar puntos de incomodidad |
| No percibir el funcionamiento del sistema | El sistema debe ser silencioso y no invasivo |
| No limitar la movilidad del cuerpo | El diseño debe permitir movimientos normales del paciente |
| Garantizar seguridad eléctrica | El dispositivo debe cumplir con la norma de seguridad eléctrica IEC 60601-1, con corrientes de fuga inferiores a 100 µA. |
| Ser higiénico y apto para uso clínico | El material debe ser lavable o contar con protección impermeable. Se puede usar para la cubierta externa poliuretano (PU) médico o TPU, o nylon/poliéster, con el fin de impermeabilizar el interior y que sea apto para el lavado.  |
| Adaptarse a distintos tipos de pacientes | El sistema debe funcionar para diferentes pesos y contexturas |
| No generar falsas alarmas molestas | El sistema debe filtrar ruido y evitar activaciones espurias |

Referencias

\[1\] A wireless, skin-integrated system for continuous pressure distribution monitoring to prevent ulcers across various healthcare environments.

\[2\] pressure monitoring approach for pressure ulcer prevention.

\[3\] A Smart IoT System for Detecting the Position of a Lying (resolution menor a 10 cm)

\[4\] ForeSite® Intelligent Surface System \- User guide (resolucion de 3 cm)  
