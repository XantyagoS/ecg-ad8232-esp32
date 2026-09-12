/*
  ECG con AD8232 + ESP32-S3  —  modo onda para el Serial Plotter.

  Manda UN numero por linea a 250 Hz, que es lo que el Serial Plotter
  del Arduino IDE sabe graficar.

  USO:
    1. Cargar este sketch.
    2. Tools -> Serial Plotter
    3. Poner 115200 baud en el desplegable de abajo a la derecha.

  Conexiones:
    AD8232 GND    -> ESP32 GND
    AD8232 3.3V   -> ESP32 3V3      (NUNCA 5V ni VIN)
    AD8232 OUTPUT -> GPIO4          (ADC1_CH3)
    AD8232 LO+    -> GPIO5
    AD8232 LO-    -> GPIO6
    AD8232 SDN    -> 3V3

  Alimentar por USB con el portatil DESCONECTADO del cargador.
*/

// 1 = manda la senal filtrada (recomendado: se ve el PQRST)
// 0 = manda el ADC crudo 0..4095 (util para verificar el cableado)
#define FILTRAR 1

// Frecuencia de la red electrica: 60 en Colombia, 50 en Europa.
#define F_RED 60.0f

const int PIN_ECG = 4;
const int PIN_LOP = 5;
const int PIN_LON = 6;

// LED RGB integrado.
// IMPORTANTE: se ataca UN SOLO pin.  Antes escribia en 5 candidatos a
// la vez y eso colgaba la placa: el ESP32-S3 tiene 4 canales RMT y
// rgbLedWrite() reserva uno por pin, asi que el quinto se quedaba sin
// canal.  Si tu LED no enciende, cambia 48 por 38 (los dos GPIO
// habituales en las placas ESP32-S3).
#define PIN_LED 48

const float FS = 250.0f;
const unsigned long PERIODO_US = 4000UL;   // 1/250 s
unsigned long t_prev = 0;

// ------------------------------------------------------------------
// Biquad en forma directa II transpuesta.
// Sin filtrar, la deriva de la linea base hace que el Plotter
// reescale todo el tiempo y el PQRST se pierde en el vaiven.
// ------------------------------------------------------------------
struct Biquad {
  float b0, b1, b2, a1, a2, z1, z2;

  void set(float B0, float B1, float B2, float A0, float A1, float A2) {
    b0 = B0 / A0;  b1 = B1 / A0;  b2 = B2 / A0;
    a1 = A1 / A0;  a2 = A2 / A0;
    z1 = z2 = 0.0f;
  }
  float operator()(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Biquad hp, notch, lp;

void disenaFiltros() {
  const float PI2 = 6.2831853f;

  // Pasa-altos 0.5 Hz: quita la deriva de la linea base.
  {
    float w = PI2 * 0.5f / FS, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    hp.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  // Notch en la frecuencia de red: quita el zumbido de 60 Hz.
  {
    float w = PI2 * F_RED / FS, c = cosf(w), al = sinf(w) / (2.0f * 30.0f);
    notch.set(1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
  }
  // Pasa-bajos 40 Hz: quita el ruido muscular de alta frecuencia.
  {
    float w = PI2 * 40.0f / FS, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    lp.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
}

float offset = -1.0f;      // nivel de reposo, para arrancar sin golpe
long  n = 0;

// ---- deteccion de pico R, solo para el destello del LED ----------
// El umbral sale del maximo del ultimo par de segundos, calculado por
// bloques de 1 s.  Un decaimiento exponencial se queda colgado alto
// despues de un artefacto y hace perder latidos.
float bloque_max = 0.0f, hist[3] = {0, 0, 0};
int   bloque_n = 0, hist_i = 0;
long  ult_pico = -1000;
unsigned long t_destello = 0, t_led = 0;

void led(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(PIN_LED, r, g, b);
}

void setup() {
  Serial.begin(115200);
  delay(1500);                 // margen para que el host abra el puerto
  pinMode(PIN_LOP, INPUT);
  pinMode(PIN_LON, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ECG, ADC_11db);
  disenaFiltros();
  led(40, 40, 40);           // gris al arrancar: senal de que hay vida
  t_prev = micros();
}

void loop() {
  // Comparacion CON SIGNO a proposito: con aritmetica unsigned, si
  // t_prev se adelanta a micros() la resta se desborda a ~4.29e9,
  // nunca es menor que PERIODO_US y la espera queda desactivada para
  // siempre (el muestreo se dispara a varios kHz).
  while ((long)(micros() - t_prev) < (long)PERIODO_US) { /* espera */ }
  t_prev += PERIODO_US;
  if ((long)(micros() - t_prev) > (long)(4 * PERIODO_US)) t_prev = micros();

  int v = analogRead(PIN_ECG);
  bool despegado = (digitalRead(PIN_LOP) == HIGH || digitalRead(PIN_LON) == HIGH);

#if FILTRAR
  if (offset < 0) offset = (float)v;         // arranca desde el reposo real
  float y = (float)v - offset;
  y = lp(notch(hp(y)));

  // El pasa-altos tarda ~1.5 s en asentarse.  Durante ese transitorio
  // se manda 0: si no, el primer pico deja el Plotter con una escala
  // absurda y no se ve nada durante el resto de la sesion.
  if (++n < (long)(1.5f * FS)) Serial.println(0);
  else                        Serial.println((int)y);

  // ---- pico R -----------------------------------------------------
  if (y > bloque_max) bloque_max = y;
  if (++bloque_n >= (int)FS) {
    hist[hist_i] = bloque_max;
    hist_i = (hist_i + 1) % 3;
    bloque_max = 0.0f;
    bloque_n = 0;
  }
  float pico = max(hist[0], max(hist[1], hist[2]));
  if (pico > 20.0f && y > 0.5f * pico && (n - ult_pico) > (long)(0.3f * FS)) {
    ult_pico = n;
    t_destello = millis();
  }
#else
  Serial.println(v);
#endif

  // ---- LED --------------------------------------------------------
  // A 20 Hz: el WS2812 no necesita mas, y a 250 Hz se desperdicia
  // tiempo del bucle de muestreo.
  if (millis() - t_led > 50) {
    t_led = millis();
    if (despegado)                        led(0, 0, 48);    // azul
    else if (millis() - t_destello < 80)  led(60, 60, 60);  // destello
    else                                  led(0, 48, 0);    // verde
  }
}
