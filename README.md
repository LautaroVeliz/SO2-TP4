# Sistemas Operativos II - Trabajo práctico IV - Real Time
## Introducción
El siguiente trabajo práctico se desarrolla con el fin de poder implementar el sistema operativo de tiempo real conocido como FreeRTOS sobre el microcontrolador Stellaris LM3S811. Esta implementación se lleva a cabo emulando el sistema a través de la herramienta qemu en un sistema operativo de tipo Linux. De este modo se puede aprender y familiarizar al estudiante con el uso de estos sistemas con el objetivo de utilizar las tareas de tiempo real brindadas por el sistema operativo para aplicaciones del mundo real que las requieran.

## Desarrollo
Para el desarrollo del proyecto se espera que este cumpla con los siguientes puntos:
- Poseer una tarea que simule un sensor de temperatura. Generando valores aleatorios, con una frecuencia de 10 Hz.
- Poseer una tarea que reciba los valores del sensor y aplique un filtro pasa bajos. Donde cada valor resultante es el promedio de las últimas N mediciones.
- Poseer una tarea que grafique en el display los valores de temperatura en el tiempo.
- Poseer una tarea tipo top de linux, que muestre periódicamente estadísticas de las tareas (uso de cpu, uso de memoria, etc).
- Se debe poder recibir comandos por la interfaz UART para cambiar el N del filtro.
Además se solicita calcular el stack necesario para cada task, realizando el análisis utilizando las funciones del sistema uxTaskGetStackHighWaterMark o vApplicationStackOverflowHook.

### Observaciones Importantes
- Temperatura: El valor de la temperatura está definido en décimas de grado Celsius, tendrá como mínimo el valor de 0 (0.0° Celsius), definido en el código como *MIN_TEMP_DECIMALS ( 0 )*, y como máximo el valor de 150 (15.0° Celsius), definido en el código como *MAX_TEMP_DECIMALS ( 150 )*; a su vez el valor inicial de temperatura será el punto medio entre el máximo y el mínimo (150 + 0) / 2 = 75 (7.5° Celsius) definido en el código como *INITIAL_TEMP_DECIMALS ( (MAX_TEMP_DECIMALS + MIN_TEMP_DECIMALS) / 2 )*.
- Colas: Existirán dos colas globales:
  - s_temps_queue: Dedicada a almacenar valores de temperatura generados por el sensor.
  - s_temps_to_display_queue: Dedicada a almacenar valores de temperatura que deberán mostrarse en la pantalla LCD.
- Número de muestras: Corresponde a la cantidad de muestras que se tomarán al momento de calcular el promedio para luego mostrar en la pantalla LCD. En el código queda representada por una variable global de tipo volatile llamada *s_numberOfSamples*. A su vez el valor de esta variable queda acotado por las definiciones de *MAX_NUMBER_OF_SAMPLES ( 20 )* y *MIN_NUMBER_OF_SAMPLES ( 1 )*.

### Sensor Task
El trabajo de esta tarea de ejecución periódica es generar simular la generación de datos de temperatura, para ello se utilizó un código de generación de datos pseudoaleatorios que usa como semilla el contador de ticks del reloj interno del microcontrolador. Este valor generado pseudo aleatoriamente está acotado dentro de un espacio cuyos límites inferior y superior se definen como el último valor de temperatura medido más menos un valor definido como *TEMP_DECIMALS_STEP* respectivamente, esto con el fin de que la variación de temperaturas entre cada dato no diverge en gran medida respecto al valor anterior, a su vez el valor final calculado como nueva temperatura será acotado entre los valores definidos como máximo y mínimo para valores de temperatura. Finalmente el nuevo valor es colocado en la cola *s_temps_queue*.

Esta tarea se ejecuta en forma periódica con una frecuencia definida en el codigo como *SENSOR_FRECUENCY_HZ ( 10 )*, por defecto 10Hz.

### Average Task
Esta tarea debe estar en constante funcionamiento intentado obtener datos desde la cola *s_temps_queue*. Cada dato obtenido en un arreglo propio el cual funciona como una cola FIFO simple de datos de temperatura históricos. A partir de este arreglo-cola se calcula el promedio de las N mediciones más recientes, con N definido por la variable *s_numberOfSamples*, y luego este valor es colocado dentro de la cola *s_temps_to_display_queue* para su posterior visualización.

### Display Task
Esta tarea es la encargada de controlar el display LCD que controla el microcontrolador. Los datos que debe mostrar son los siguientes:
- El número de muestras con el cual se calcula el promedio de temperaturas con un formato de **N=xx**.
- Una línea vertical que se dibuje a través de las 16 líneas horizontales que otorga la pantalla LCD, esta línea representa un eje de ordenadas cuyos 16 valores posicionales representan al conjunto [0,...,15] (los valores que la temperatura puede tomar), así como como un pequeño 0 que se hallara a la izquierda de la línea y próxima al valor 0 de la misma, representando el punto de origen de este gráfico.
- Los valores de temperatura a través del tiempo, cada uno representado por un punto en el gráfico. Cabe aclarar que por el uso de parte de la pantalla para los datos mencionados anteriormente, el número de columnas que quedan disponibles para la muestra de datos es de 69 definido en el código como *LCD_COLUMNS_FOR_GRAPH   ( 69 )*.
Para llevar a cabo lo mencionado, lo primero que hace es esperar a que haya datos disponibles en la cola *s_temps_to_display_queue* y al tomar un nuevo valor a mostrar lo coloca en su array (también es tipo cola FIFO) de tamaño *LCD_COLUMNS_FOR_GRAPH*, como siguiente paso convierte el valor de *s_numberOfSamples* en dos caracteres (xx) los cuales se colocan dentro de una cadena a ser mostrada *"N=xx"*. Luego imprime la línea vertical junto a su punto de origen.

Finalmente se recorre el arreglo de datos a mostrar calculando el valor que debe tener la columna para mostrar el valor correspondiente y, en caso de que el valor sea diferente a su antecesor, se dibuja una línea que ocupa desde el valor próximo más cercano en dirección al nuevo valor hasta el nuevo valor. Ej: Supongamos la siguiente secuencia de valores 11 - 14 - 14 - 12 - 12, el gráfico quedaría de la siguiente forma:

|||||||
|-|-|-|-|-|-|
|14| |#|#|||
|13| |#||#||
|12| |#||#|#|
|11| #|||||

### Top Task
Esta tarea en lapsos de tiempo definidos por *TOP_TASK_DELAY_MS*, en cada iteración la tarea recoge información como tiempo de ejecución total y uso del stack (tamaño del espacio total que posee, máximo espacio utilizado, y espacio utilizado en el momento de la medición) de todas las tareas, y además obtiene información del espacio de heap de la memoria. La información referente a las tareas es mostrada en forma de tabla con una fila para cada tarea y debajo de esta tabla se halla la información referente a la heap del sistema. 

Los datos de las tareas son obtenidos mediante la función *px_task_status_array* que carga en un arreglo de estructura *TaskStatus_t* una estructura para cada tarea. De aquí se obtiene los datos para calcular el tamaño de la stack (no coincide con el asignado por usos internos del sistema para guardar retornos por ejemplo), también se puede hallar la posición actual del puntero de la cima del stack lo cual permite saber cuando se está utilizando en el momento de llamar a la función y finalmente el mínimo espacio libre de stack que tuvo en algún momento la tarea. Cabe aclarar que esto reemplaza el llamado a las funciones *uxTaskGetStackHighWaterMark* o *vApplicationStackOverflowHook*, permitiendo obtener la misma información. 

A su vez la tarea guarda el último valor del tiempo de ejecución para poder compararlo en la siguiente iteración con el valor actual y calcular el tiempo de ejecución de cada tarea durante el lapso de espera de la tarea.

Con respecto a los datos de la heap se puede obtener el tamaño total desde el define *configTOTAL_HEAP_SIZE* en el archivo *FreeRTOSConfig.h* y utilizando la función *xPortGetFreeHeapSize* se obtiene el espacio libre en la heap, con una simple resta podemos obtener a su vez el valor de la heap en uso. Estos datos son mostrados por la tarea debajo de la tabla de tareas.

Nota: La tarea al crearse queda en estado de suspendida, y será activada al utilizar el comando **top** via UART, como se explica más adelante.

### Interrupt UART
Este handler se ocupa de dos tareas, actualizar el valor de *s_numberOfSamples* y activar, o suspender, la tarea **top**. Para esto recibe los caracteres que son enviados via UART y los almacena en un buffer de tamaño 3 bytes, esto es ya que los comando aceptados son *top*, *Nx* y *Nxx*, a su vez refleja los caracteres por el transmisor de UART para que aparezcan en la terminal desde la cual se está comunicando el usuario. Cualquier carácter extra será leído pero no se tendrá en cuenta para el comando. Cuando llega un caracter '\n' (salto de línea, por presionar enter) el handler debe analizar el contenido del buffer y responder ya se con la acción solicitada o con un mensaje de *"Invalid command"*.
En el caso de que el comando enviado sea *top*, comenzará a ejecutarse la tarea top, y ningún carácter que sea enviado se almacenará o se reflejará, excepto el caso del carácter 'q' (símbolo de quit), en este caso si bien no se reflejará, se verá suspendida la tarea top y el comportamiento de la UART volverá a la normalidad.
En el caso de los comandos *Nx* y *Nxx*, el handler actualizará el valor de *s_numberOfSamples* en el valor de *x* o *xx* según sea el caso. Siempre respetando que estos valores se hallen en intervalo [1,...,20] y si no es así se asignará el extremo del intervalo más próximo al valor enviado.

### Timer0
Para poder ejecutar correctamente la tarea top se necesita poder medir el tiempo de ejecución de una tarea en un lapso de tiempo, esta estadística es controlada por tres defines hallados en *FreeRTOSConfig.h*:
- configGENERATE_RUN_TIME_STATS 1 
  - Al asignarle el valor 1 se habilita el acceso a estadísticas de tiempo de ejecución
- portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ( vSetupRunTimeStatsTimer() )
  - Este define la función que configura el timer que se usará para la estadística
- portGET_RUN_TIME_COUNTER_VALUE() ( ulGetRunTimeCounterValue() ) 
  - Y esta define desde donde se puede obtener el valor actual del contador usado para la estadística
  
Teniendo en cuenta esto se configuró el Timer0A, para que ejecute cuentas de **TIMER_TICK_COUNT** (constante definida) ticks (estos ticks son los ejecutados por el Clk del sistema), a su vez al llegar a 0 el handler del Timer0A incrementa la variable global *s_overflow_counter* y reinicia el conteo atrás del Timer. El valor de **TIMER_TICK_COUNT** está definido para que los ticks del Timer0A se realicen en periodos de 10uS, el cálculo es el siguiente: X Hz * 10uS = X Hz / 10.000 = **TIMER_TICK_COUNT** ticks. Por ello **TIMER_TICK_COUNT** equivale a *( configCPU_CLOCK_HZ / 10000 )*.

## Utilización
Para poder correr el proyecto se debe ingresar al directorio *./Demo/CORTEX_LM3S811_GCC* dentro del proyecto y ejecutar el ejecutale *run.sh*. De esta manera se compilaran los archivos necesarios y qemu emulara el comportamiento del microcontrolador. A partir de aqui, como ya se menciono, los comando disponibles son:
- **top**: Para la ejecucion de la tarea tipo top.
- **Nxx**: Para la variacion del numero de muestras a tomar en el promedio del filtro de pasa bajo para la exibicion en la pantalla LCD simulada.
