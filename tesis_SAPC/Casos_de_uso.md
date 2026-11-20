Se definen los siguientes casos de uso en base a las necesidades del usuario.

### **CU1: Monitoreo y Procesamiento Continuo de Presión**

**Actores:** Sistema (hardware, firmware y software), Base de datos. 

**Descripción:** Es el proceso central e invisible para el usuario, donde el sistema adquiere los datos físicos, los filtra y determina si se están cumpliendo las condiciones de riesgo de isquemia. 

**Precondiciones:** El sistema debe estar encendido, posicionado sobre la cama y el paciente acostado sobre él.

**Flujo Principal:**

1. El microcontrolador muestrea continuamente los sensores a una frecuencia de 1000 Hz (muestreo  de los sensores).  
2. El sistema lee las tres matrices de 32 sensores cada una, respetando la resolución espacial de aproximadamente 50 mm entre sensores (para el prototipo se utilizará una sola matriz).  
3. El hardware debe poder ser capaz realizar un preprocesamiento de la señal (correción de offset, preamplificación y filtrado).  
4. El firmware procesa y envía los datos. El procesamiento del firmware debe aplicar un algoritmo de detección de puntos críticos.  
5. El sistema evalúa continuamente los valores máximos sensados por fila o columna en cada matriz.  
6. Se calcula si la presión actual supera el umbral de riesgo, establecido entre 1/3 y 3/5 del valor máximo.  
7. El sistema inicia un temporizador interno si se supera el umbral, verificando si esta condición se mantiene por un período de 1:30 hs a 2:00 hs o se superó.  
8. El sistema condensa y promedia los datos para establecer una frecuencia de actualización (muestreo del sistema) de 0,1 Hz, es decir, una actualización cada 10 segundos. Se debe tener en cuenta la cantidad y peso de datos por envío.  
9. Los datos procesados se empaquetan y se envían a la base de datos (como InfluxDB, MongoDB o SQLite) para su registro histórico.

**Flujos Alternativos:**

* **Desconexión temporal del paciente:** Si el sistema detecta una caída abrupta de presión en todas las matrices o una de ellas (indicando que el paciente se levantó o está siendo bañado), pausa el temporizador de riesgo para contemplar estas rutinas externas.

### **CU2: Emisión de Alerta local de Riesgo** 

**Actores:** Sistema, Enfermero/Cuidador.

**Descripción:** Es el mecanismo de respuesta física e inmediata del dispositivo ubicado en la cama para notificar al personal que se encuentra en la habitación sobre el estado de riesgo del paciente.

**Precondiciones:** El sistema debe estar procesando los datos (CU1 en ejecución) y contar con alimentación eléctrica.

**Flujo Principal:**

1. El sistema detecta que el tiempo de permanencia sobre un punto de alta presión superó el tiempo máximo recomendado (ej. 1:30 hs).  
2. El sistema enciende el indicador luminoso secundario (LED de criticidad) en el módulo físico para señalar de forma rápida y visual el riesgo detectado.  
3. El cuidador acude a la cama del paciente y realiza el protocolo de rotación.  
4. El sistema detecta la redistribución de presiones al cambiar de postura, apaga el indicador de criticidad y reinicia el ciclo de monitoreo continuo de los puntos de presión sin requerir intervención manual.

**Flujos Alternativos:**

* **Fallo de comunicación:** Aunque el sistema pierda temporalmente la conexión Wi-Fi, el módulo físico debe garantizar la ejecución de este caso de uso de manera autónoma para no dejar desprotegido al paciente. Debería llevar una cuenta del tiempo de desconexión y una alarma asociada (LED de conexión).

### **CU3: Visualización del Estado del Paciente e Historial**

**Actores:** Enfermero/Cuidador, Supervisor. 

**Descripción:** Interacción principal del personal médico con la interfaz de software para auditar, revisar y anticipar los cambios posturales de los pacientes. 

**Precondiciones:** El sistema debe estar transmitiendo datos vía Wi-Fi hacia la base de datos y la interfaz web debe estar operativa.

**Flujo Principal:**

1. El usuario accede a la interfaz desde una laptop o computadora de escritorio.  
2. La interfaz muestra datos de identificación del paciente y con la recomendación de rotación con un horario específico estimado  
3. En otro menú, el usuario accede a un mapa de presión simplificado y observa el estado de riesgo general codificado por colores: normal, alerta o crítico.  
4. El usuario verifica una barra de tiempo gráfica que muestra la recomendación de rotación con un horario específico estimado.  
5. El usuario navega hacia el menú de historial de posiciones.  
6. La interfaz despliega un listado de las zonas críticas o de riesgo registradas por día y por semana.  
7. La interfaz informa estadísticas clave, como el promedio de tiempo que el paciente pasó en cada zona crítica y el tiempo medio de rotación acumulado en un rango de días.

### **CU4: Gestión del Módulo Físico y Conectividad**

**Actores:** Enfermero/Cuidador, Técnico de Instalación.

**Descripción:** Abarca la colocación mecánica del dispositivo, su encendido y la vinculación a la red de la clínica.

**Precondiciones:** El cubrecolchón debe estar dispuesto para su uso y debe existir una red Wi-Fi disponible en las instalaciones.

**Flujo Principal:**

1. El usuario posiciona el cubrecolchón de manera fácil sobre el colchón de la cama, ajustando las correas de fijación si corresponde.  
2. El usuario presiona el único botón de encendido ubicado en el módulo físico.  
3. El módulo confirma el encendido iluminando el LED RGB principal en color rojo.  
4. El LED RGB cambia a color amarillo intermitente, indicando que el módulo está en proceso de conexión a la red Wi-Fi.  
5. Una vez establecida la conexión, el LED RGB cambia a color azul de forma fija.

**Flujos Alternativos:**

* **Primera conexión:** El técnico se encarga de realizar y configurar la primera conexión a una red wifi disponible.  
* **Cambio de red Wi-Fi:** Si la red de la clínica cambia de contraseña o SSID, el usuario sigue una guía proporcionada con el equipo para reconfigurar las credenciales de red de manera intuitiva, sin necesidad de recurrir al técnico instalador.

