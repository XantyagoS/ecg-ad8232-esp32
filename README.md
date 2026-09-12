# ECG con AD8232 y ESP32-S3

Adquisición y visualización de un electrocardiograma (onda P-QRS-T) con
un módulo **AD8232** y una placa **ESP32-S3**, muestreando a 250 Hz.

Proyecto de bioingeniería — Unisangil.

> ⚠️ Montaje educativo. **No sirve para diagnóstico médico.**
> Alimentar siempre con el portátil desconectado del cargador, o con
> batería / power bank. Nunca con el equipo enchufado a la red eléctrica
> mientras haya electrodos sobre una persona.

## Hardware

| Componente | Notas |
|---|---|
| ESP32-S3 | DevKit con PSRAM octal de 8 MB |
| AD8232 | Módulo de monitorización cardíaca de una derivación |
| Electrodos Ag/AgCl | Desechables, de broche |
| Cable de 3 latiguillos | El que trae jack de 3.5 mm |

## Conexiones

| AD8232 | ESP32-S3 | Por qué |
|---|---|---|
| GND | `GND` | Referencia |
| 3.3V | `3V3` | **Nunca 5V ni VIN** (el AD8232 aguanta 3.5 V máximo) |
| OUTPUT | `GPIO4` | Señal analógica (ADC1_CH3) |
| LO+ | `GPIO5` | Detección de electrodo despegado |
| LO- | `GPIO6` | Detección de electrodo despegado |
| SDN | `3V3` | Mantiene el chip activo; al aire se apaga de forma intermitente |

**Los pines importan.** En el ESP32-S3 no se pueden usar los GPIO32/34/35
del ESP32 clásico: no existen o están ocupados. Los GPIO26–37 se los queda
el flash y la PSRAM, los GPIO19/20 son el USB nativo, y los GPIO0/3/45/46
son de strapping. Los GPIO1–10 son ADC1, el único ADC que sigue funcionando
con WiFi encendido.

### Colocación de electrodos (derivación II)

| Color | Posición |
|---|---|
| 🔴 Rojo (RA) | Debajo de la clavícula derecha |
| 🟡 Amarillo (LA) | Costado izquierdo, costillas bajas |
| 🟢 Verde (RL) | Abdomen inferior derecho (referencia) |

Limpiar la piel con alcohol y esperar de 30 a 60 s tras pegarlos, para que
el gel estabilice su potencial de media celda.

## Firmware

Compilar con `Board: ESP32S3 Dev Module`, `PSRAM: OPI PSRAM`,
`USB CDC On Boot: Enabled` (si se usa el conector USB nativo).

| Sketch | Para qué |
|---|---|
| **`ecg_plotter/`** | **El principal.** Manda un número por línea a 250 Hz, filtrado (pasa-altos 0.5 Hz + notch de red + pasa-bajos 40 Hz). Se ve en el Serial Plotter del Arduino IDE a 115200 baudios. Incluye LED de estado y destello por latido. |
| `paso1_minimo/` | Diagnóstico. Imprime una línea legible cada 5 s con el rango del ADC y el estado de contacto. Útil para verificar el cableado sin electrodos. |
| `ecg_ad8232_esp32/` | Versión con autodiagnóstico de cableado por LED RGB. **Se cuelga**: escribe en 5 pines de LED y el ESP32-S3 solo tiene 4 canales RMT. Se conserva como referencia del método de detección de pin al aire. |
| `ecg_ad8232_ARDUINO_UNO_no_usar/` | Versión original para Arduino UNO. Pines incompatibles con el ESP32-S3. |

### Detección de pin al aire

`ecg_ad8232_esp32` incluye una técnica útil: leer un pin con el pull-up
interno y luego con el pull-down. Si algo lo está manejando, ambas lecturas
coinciden; si el jumper está suelto, el pin sigue a la resistencia interna y
difieren. Es la única forma de distinguir un cable flojo de un nivel bajo
legítimo.

## Visualización

### Serial Plotter del Arduino IDE

Cargar `ecg_plotter`, abrir `Tools → Serial Plotter` y poner **115200 baud**.

### Terminal

```bash
stty -f /dev/cu.usbmodem101 115200 raw -echo
cat /dev/cu.usbmodem101
```

### Visor en terminal con detección de latidos

```bash
python3 ecg_term.py           # sin dependencias, solo librería estándar
python3 ecg_term.py --demo    # señal PQRST sintética, sin hardware
```

Dibuja la onda, marca los picos R y calcula la frecuencia cardíaca.

### Gráfica con matplotlib

```bash
pip install pyserial matplotlib numpy scipy
python3 plot_ecg.py
```

Guarda la captura en `ecg.csv` al cerrar la ventana.

## Notas de implementación

**El temporizador usa comparación con signo a propósito.** Con aritmética
unsigned, si `t_prev` llega a adelantarse a `micros()` la resta se desborda a
~4.29e9, que nunca es menor que el periodo, y la espera queda desactivada
para siempre: el muestreo se dispara a varios kHz y todos los filtros quedan
calculados para una frecuencia equivocada.

**El filtro pasa-altos necesita ~1.5 s para asentarse.** Durante ese
transitorio se manda cero; si no, el pico inicial deja la escala del Plotter
arruinada para el resto de la sesión.
