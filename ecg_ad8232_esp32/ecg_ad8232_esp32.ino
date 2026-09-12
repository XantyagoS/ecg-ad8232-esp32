/*
  ECG con AD8232 + ESP32-S3  —  modo diagnostico legible.

  Muestrea a 250 Hz internamente, pero SOLO imprime una linea cada
  5 segundos, para poder leerla en la terminal sin que se inunde.

  Conexiones:
    AD8232 GND    -> ESP32 GND
    AD8232 3.3V   -> ESP32 3V3
    AD8232 OUTPUT -> GPIO4     (ADC1_CH3)
    AD8232 LO+    -> GPIO5
    AD8232 LO-    -> GPIO6
    AD8232 SDN    -> 3V3       (si se deja al aire, el chip se apaga solo)

  LED integrado:
    ROJO parpadeando : AD8232 no detectado (jumper suelto o sin 3V3/GND)
    AZUL fijo        : modulo OK, electrodos despegados
    VERDE fijo       : midiendo

  En Arduino IDE:  Board "ESP32S3 Dev Module", PSRAM "OPI PSRAM",
  USB CDC On Boot "Enabled" (puerto /dev/cu.usbmodem101).
*/

const int PIN_ECG = 4;    // ADC1_CH3
const int PIN_LOP = 5;    // Leads-Off +
const int PIN_LON = 6;    // Leads-Off -

const unsigned long PERIODO_US = 4000UL;    // 250 Hz
const unsigned long INFORME_MS = 5000UL;    // una linea cada 5 s

// El WS2812 integrado esta en un GPIO distinto segun la placa.  En vez
// de averiguar cual, se escribe en todos los candidatos a la vez: el
// que exista se enciende y los demas no hacen nada.  No se incluyen
// 4, 5 ni 6, que son los del AD8232.
const int PINES_LED[] = {48, 38, 47, 21, 2};
const int N_LED = sizeof(PINES_LED) / sizeof(PINES_LED[0]);

unsigned long t_prev = 0, t_informe = 0, t_led = 0;
bool hay_modulo = false;

// Estadisticas del intervalo de 5 s
long   suma = 0;
int    n_muestras = 0, v_min = 4095, v_max = 0, n_despegado = 0;

void led(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < N_LED; i++) rgbLedWrite(PINES_LED[i], r, g, b);
}

// ------------------------------------------------------------------
// Deteccion de pin al aire: se lee con el pull interno hacia arriba y
// luego hacia abajo.  Si el AD8232 lo esta manejando, gana el modulo y
// las dos lecturas coinciden.  Si el jumper esta suelto, el pin sigue
// al pull interno y difieren.  Es la unica forma de distinguir un
// cable flojo de un nivel bajo legitimo.
// ------------------------------------------------------------------
bool pinManejado(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(300);
  int arriba = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(300);
  int abajo = digitalRead(pin);
  pinMode(pin, INPUT);
  return arriba == abajo;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LOP, INPUT);
  pinMode(PIN_LON, INPUT);
  analogReadResolution(12);                    // 0..4095
  analogSetPinAttenuation(PIN_ECG, ADC_11db);  // rango ~0..3.3V
  led(40, 40, 40);
  delay(300);
  Serial.println();
  Serial.println("ECG AD8232 + ESP32-S3 — informe cada 5 s");
  Serial.println("----------------------------------------");
  t_prev = micros();
  t_informe = millis();
}

void loop() {
  // Comparacion CON SIGNO a proposito: con aritmetica unsigned, si
  // t_prev se adelanta a micros() la resta se desborda a ~4.29e9,
  // nunca es menor que PERIODO_US y la espera queda desactivada para
  // siempre (el muestreo se dispara a varios kHz).
  while ((long)(micros() - t_prev) < (long)PERIODO_US) { /* espera */ }
  t_prev += PERIODO_US;
  if ((long)(micros() - t_prev) > (long)(4 * PERIODO_US)) t_prev = micros();

  bool despegado = (digitalRead(PIN_LOP) == HIGH || digitalRead(PIN_LON) == HIGH);
  int v = analogRead(PIN_ECG);

  n_muestras++;
  suma += v;
  if (v < v_min) v_min = v;
  if (v > v_max) v_max = v;
  if (despegado) n_despegado++;

  if (millis() - t_led > 100) {
    t_led = millis();
    if (!hay_modulo)      led((millis() / 400) % 2 ? 48 : 0, 0, 0);   // rojo
    else if (despegado)   led(0, 0, 48);                              // azul
    else                  led(0, 48, 0);                              // verde
  }

  if (millis() - t_informe >= INFORME_MS) {
    t_informe += INFORME_MS;

    // La prueba con pulls perturba la entrada, asi que solo se hace
    // aqui, una vez cada 5 s, y no en plena adquisicion.
    hay_modulo = pinManejado(PIN_LOP) && pinManejado(PIN_LON);
    pinMode(PIN_LOP, INPUT);
    pinMode(PIN_LON, INPUT);

    int med = n_muestras ? (int)(suma / n_muestras) : 0;
    Serial.printf("[%4lu s] %-14s  %-12s  ADC min=%4d med=%4d max=%4d  (%d muestras, %d Hz)\n",
                  millis() / 1000,
                  hay_modulo ? "modulo OK" : "SIN MODULO",
                  n_despegado > n_muestras / 2 ? "sin contacto" : "con contacto",
                  v_min, med, v_max,
                  n_muestras, n_muestras / (int)(INFORME_MS / 1000));

    suma = 0; n_muestras = 0; v_min = 4095; v_max = 0; n_despegado = 0;
  }
}
