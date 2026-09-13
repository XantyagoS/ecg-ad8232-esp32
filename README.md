# ECG con AD8232 y ESP32-S3 con pantalla TFT

Electrocardiógrafo de una derivación: adquiere la señal de un módulo
**AD8232** con un **ESP32-S3** a 250 Hz y la muestra en una pantalla
**TFT redonda GC9A01 de 240×240**, con la onda P-QRS-T, la frecuencia
cardíaca y el intervalo R-R.

Proyecto de bioingeniería — Unisangil.

> ⚠️ Montaje educativo. **No sirve para diagnóstico médico.**
> Alimentar siempre con el portátil desconectado del cargador, o con
> batería. Nunca con el equipo enchufado a la red eléctrica mientras
> haya electrodos sobre una persona.

## Hardware

| Componente | Notas |
|---|---|
| ESP32-S3 | DevKit con PSRAM octal de 8 MB |
| AD8232 | SparkFun Heart Monitor, una derivación |
| TFT GC9A01 | 1,28" redonda, 240×240, SPI |
| Electrodos Ag/AgCl | Desechables, de broche |
| Protoboard | Imprescindible: hacen falta 3 conexiones a 3,3 V y la placa solo tiene 2 pines |

## Conexiones

### AD8232 → ESP32-S3

| Pin | Va a |
|---|---|
| GND | `GND` |
| 3.3V | `3V3` |
| OUTPUT | `GPIO4` (ADC1_CH3) |
| LO− | `GPIO5` |
| LO+ | `GPIO6` |
| SDN | *sin conectar* |

El módulo trae los pines en el orden `GND · 3.3v · OUTPUT · LO− · LO+ · SDN`:
**LO− viene antes que LO+**, al contrario de lo que suele listarse.

`SDN` va al aire porque el breakout de SparkFun lo trae con pull-up.

### TFT GC9A01 → ESP32-S3

| Pin del módulo | Va a |
|---|---|
| VCC | `3V3` |
| GND | `GND` |
| SCK | `GPIO12` |
| SDA | `GPIO11` |
| CS | `GPIO10` |
| A0/DC | `GPIO14` |
| RESET | *sin conectar* |
| LED | `3V3` |

Tres rótulos de este módulo engañan:

- **`SDA` no es I²C.** Es **MOSI**, la línea de datos del SPI.
- **`LED` es la retroiluminación.** Sin ella la pantalla funciona pero se ve negra.
- **`RESET` se puede omitir:** la librería manda un reset por software.

### Reparto de la alimentación

Son **3 conexiones a 3,3 V** (AD8232 VCC, TFT VCC, TFT LED) y el ESP32-S3
tiene 2 pines `3V3`. Se resuelve con la protoboard: al montar la placa,
cada pin ocupa un hueco de su fila y **deja 4 libres, todos unidos por
dentro**.

```
fila del pin 3V3:
     a          b          c         d        e
 [pin ESP32][AD8232 VCC][TFT VCC][TFT LED][libre]
```

### Pines que no se pueden usar en el ESP32-S3

No son los del ESP32 clásico. GPIO26–37 se los queda el flash y la PSRAM
octal, GPIO19/20 son el USB nativo, GPIO43/44 el UART0, y GPIO0/3/45/46
son de strapping. **GPIO1–10 son ADC1**, el único ADC que sigue vivo con
WiFi encendido. `GPIO13` se deja libre a propósito: es el MISO por
defecto y hace falta si algún día se usa la ranura microSD de la pantalla.

## Colocación de electrodos (derivación II)

| Color | Posición |
|---|---|
| 🔴 Rojo (RA) | Debajo de la clavícula derecha |
| 🟡 Amarillo (LA) | Costado izquierdo, costillas bajas |
| 🟢 Verde (RL) | Abdomen inferior derecho (referencia) |

Limpiar la piel con alcohol y esperar de 30 a 60 s tras pegarlos, para
que el gel estabilice su potencial de media celda.

## Qué muestra

La pantalla y el LED RGB de la placa van sincronizados:

| Estado | Pantalla | LED |
|---|---|---|
| Iniciando | Aro gris · "ECG AD8232" | 🔘 Gris |
| Listo, sin medir | Aro azul · "LISTO — coloca los electrodos" | 🔵 Azul |
| Midiendo | Aro verde · onda, FC y R-R | 🟢 Verde |
| Cada latido | — | ⚪ Destello blanco |

El destello por latido es la forma más rápida de saber si está captando:
si parpadea al ritmo del pulso, la señal es buena.

## Compilar y cargar

Librería: **GFX Library for Arduino** (`Arduino_GFX`), desde el gestor de
librerías del IDE.

Ajustes de placa:

```
Board            : ESP32S3 Dev Module
PSRAM            : OPI PSRAM
USB CDC On Boot  : Enabled      (con el conector USB nativo)
```

`USB CDC On Boot` depende de cuál de los dos conectores USB se use: en el
**USB nativo** (VID 0x303A) va en `Enabled`; en el del **puente CH343**
(VID 0x1A86) va en `Disabled`. Con el valor equivocado compila y carga
bien, pero el monitor serie sale vacío.

## Salida por Serial

El firmware sigue enviando la señal filtrada a 115200 bd, un número por
línea. Para verla como gráfica: `Tools → Serial Plotter`, a 115200.

Desde la terminal:

```bash
stty -f /dev/cu.usbmodemXXXX 115200 raw -echo
cat /dev/cu.usbmodemXXXX
```

El nombre del puerto cambia al reconectar; se consulta con `ls /dev/cu.*`.

## Notas de implementación

**Dos núcleos.** Un cuadro completo de 240×240 en color son 115 200 bytes;
por SPI tarda decenas de milisegundos, y el periodo de muestreo es de 4 ms.
Dibujar dentro del bucle de adquisición se comería muestras. El **núcleo 0
solo muestrea** y el **núcleo 1 solo dibuja**, comunicados por un buffer
circular de un productor y un consumidor, que no necesita semáforo.

**El muestreo usa `vTaskDelayUntil`, no espera activa.** El tick de FreeRTOS
es de 1 ms, así que 4 ticks dan 250 Hz exactos sin bloquear el núcleo ni
disparar el watchdog.

**El SPI va a 10 MHz, no a 40.** Los cables de protoboard son largos y sin
blindaje; por encima de 10–20 MHz el bus corrompe datos y la pantalla se
llena de líneas aleatorias.

**Un solo pin de LED.** El ESP32-S3 tiene 4 canales RMT y `rgbLedWrite()`
reserva uno por pin. Atacar 5 pines candidatos a la vez dejaba al quinto
sin canal y colgaba la placa antes de llegar a imprimir nada.

**Al decimar de 250 Hz a ~83 columnas por segundo se toma la muestra de
mayor amplitud de cada grupo de 3**, no la primera. El QRS dura unas pocas
muestras: tomando la primera se perdería en la mitad de los latidos.

**El transitorio inicial se silencia.** El filtro pasa-altos tarda ~1,5 s
en asentarse y su pico inicial fijaría la escala de la pantalla y del
Serial Plotter para el resto de la sesión.

**La frecuencia cardíaca se borra a los 2 s sin contacto.** Dejar el último
valor en pantalla haría creer que sigue midiendo.

## Cadena de procesamiento

```
Electrodos → AD8232 → ADC GPIO4 → pasa-altos 0,5 Hz → notch 60 Hz
  ≈1 mV      0–3,3 V   0–4095       (deriva)          (red eléctrica)

  → pasa-bajos 40 Hz → detección de pico R → pantalla + Serial
      (ruido muscular)   (umbral adaptativo)
```

El notch está en 60 Hz (Colombia). Para 50 Hz, cambiar `F_RED` en el sketch.
