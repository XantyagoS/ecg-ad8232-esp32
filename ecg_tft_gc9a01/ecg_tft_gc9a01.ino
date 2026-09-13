/*
  ECG con AD8232 + ESP32-S3 y pantalla TFT redonda GC9A01 (240x240).

  La pantalla actua de testigo con tres estados bien distintos:
    INICIANDO : aro gris,  "ECG AD8232"        -> la pantalla vive
    LISTO     : aro azul,  "LISTO"             -> conectado, sin medir
    MIDIENDO  : aro verde, onda + FC + R-R     -> midiendo

  Sigue enviando la senal filtrada por Serial a 115200 bd, un numero
  por linea, para verla en el Serial Plotter del Arduino IDE.

  ---------------------------------------------------------------
  CONEXIONES

  AD8232                 ESP32-S3
    GND        ------>   GND
    3.3V       ------>   3V3
    OUTPUT     ------>   GPIO4      (ADC1_CH3)
    LO-        ------>   GPIO5
    LO+        ------>   GPIO6
    SDN        ------>   sin conectar (el modulo trae pull-up)

  TFT (conector rotulado "TFT")
    VCC        ------>   3V3
    GND        ------>   GND
    SCK        ------>   GPIO12
    SDA        ------>   GPIO11     (es MOSI; el modulo lo rotula mal)
    CS         ------>   GPIO10
    A0/DC      ------>   GPIO14
    RESET      ------>   sin conectar (reset por software)
    LED        ------>   3V3

  ---------------------------------------------------------------
  POR QUE DOS NUCLEOS

  Un cuadro completo de 240x240 en color son 115 200 bytes.  Por SPI
  tarda decenas de milisegundos y el periodo de muestreo es de 4 ms:
  dibujar dentro del bucle de adquisicion se comeria muestras.

  Nucleo 0 solo muestrea, nucleo 1 solo dibuja.  Se comunican por un
  buffer circular de un productor y un consumidor, que no necesita
  semaforo.  El muestreo usa vTaskDelayUntil: el tick de FreeRTOS es
  de 1 ms, asi que 4 ticks dan 250 Hz exactos sin espera activa.
*/

#include <Arduino_GFX_Library.h>

// ---------------------------------------------------------------- pines
const int PIN_ECG = 4;
const int PIN_LOA = 5;        // uno de los leads-off
const int PIN_LOB = 6;        // el otro

// LED RGB integrado de la placa.  UN SOLO pin: el ESP32-S3 tiene 4
// canales RMT y rgbLedWrite() reserva uno por pin.
#define PIN_LED_PLACA 48

#define TFT_SCK  12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC   14

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
// RESET sin cablear: la libreria manda un reset por software.
Arduino_GFX *gfx = new Arduino_GC9A01(bus, GFX_NOT_DEFINED, 0, true /* IPS */);

// ---------------------------------------------------------------- senal
const float FS = 250.0f;
#define F_RED 60.0f          // 60 Hz en Colombia, 50 en Europa

struct Biquad {
  float b0, b1, b2, a1, a2, z1, z2;
  void set(float B0, float B1, float B2, float A0, float A1, float A2) {
    b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0;
    a1 = A1 / A0; a2 = A2 / A0; z1 = z2 = 0.0f;
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
  { float w = PI2 * 0.5f / FS, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    hp.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al); }
  { float w = PI2 * F_RED / FS, c = cosf(w), al = sinf(w) / (2.0f * 30.0f);
    notch.set(1, -2 * c, 1, 1 + al, -2 * c, 1 - al); }
  { float w = PI2 * 40.0f / FS, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    lp.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al); }
}

// ------------------------------------------- buffer circular entre nucleos
#define NBUF 1024
volatile int16_t  rbuf[NBUF];
volatile uint32_t w_idx = 0;      // solo lo toca el nucleo 0
uint32_t          r_idx = 0;      // solo lo toca el nucleo 1

volatile float g_bpm = 0.0f;
volatile float g_rr  = 0.0f;
volatile bool     g_contacto = false;
volatile uint32_t g_latido = 0;      // millis() del ultimo pico R

// ---------------------------------------------------------- nucleo 0
void tareaMuestreo(void *)
{
  float offset = -1.0f;
  long  n = 0, sin_contacto = 0;

  float bloque_max = 0.0f, hist[3] = {0, 0, 0};
  int   bloque_n = 0, hist_i = 0;
  long  ult_pico = -1000;

  TickType_t t = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&t, pdMS_TO_TICKS(4));      // 250 Hz exactos

    int  v = analogRead(PIN_ECG);
    bool despegado = (digitalRead(PIN_LOA) == HIGH || digitalRead(PIN_LOB) == HIGH);
    g_contacto = !despegado;

    // Sin contacto sostenido, borrar la FC: dejar el ultimo valor en
    // pantalla haria creer que sigue midiendo.
    if (despegado) {
      if (++sin_contacto > (long)(2 * FS)) { g_bpm = 0.0f; g_rr = 0.0f; }
    } else {
      sin_contacto = 0;
    }

    if (offset < 0) offset = (float)v;
    float y = lp(notch(hp((float)v - offset)));

    // El pasa-altos tarda ~1,5 s en asentarse; su transitorio inicial
    // es un pico enorme que arruinaria la escala de la pantalla.
    bool listo = (++n >= (long)(1.5f * FS));
    int16_t s = listo ? (int16_t)y : 0;

    rbuf[w_idx % NBUF] = s;
    w_idx++;
    Serial.println(s);

    if (listo && !despegado) {
      if (y > bloque_max) bloque_max = y;
      if (++bloque_n >= (int)FS) {
        hist[hist_i] = bloque_max;
        hist_i = (hist_i + 1) % 3;
        bloque_max = 0.0f; bloque_n = 0;
      }
      float pico = max(hist[0], max(hist[1], hist[2]));
      if (pico > 20.0f && y > 0.5f * pico && (n - ult_pico) > (long)(0.3f * FS)) {
        if (ult_pico > 0) {
          float rr = (n - ult_pico) / FS;
          if (rr > 0.3f && rr < 2.0f) {
            g_rr  = (g_rr > 0.0f) ? (0.75f * g_rr + 0.25f * rr) : rr;
            g_bpm = 60.0f / g_rr;
          }
        }
        ult_pico = n;
        g_latido = millis();
      }
    }
  }
}

// ---------------------------------------------------------- nucleo 1
#define NEGRO 0x0000
#define BANDA_X0 36
#define BANDA_X1 204
#define BANDA_Y0 132
#define BANDA_Y1 196
#define BANDA_MED ((BANDA_Y0 + BANDA_Y1) / 2)
#define BANDA_W  (BANDA_X1 - BANDA_X0)
#define BANDA_H  (BANDA_Y1 - BANDA_Y0)
#define DECIMA   3        // 1 columna cada 3 muestras -> barrido de ~2 s

uint16_t VERDE, AZUL, BLANCO, GRIS;

enum Vista { V_INICIO, V_ESPERA, V_MIDE };

void aro(uint16_t c) {
  for (int r = 116; r <= 118; r++) gfx->drawCircle(120, 120, r, c);
}

// Centra con la fuente integrada: 6 px de avance por caracter y tamano.
void centra(const char *s, int y, int tam, uint16_t color) {
  gfx->setTextSize(tam);
  gfx->setTextColor(color);
  gfx->setCursor(120 - (int)strlen(s) * 3 * tam, y);
  gfx->print(s);
}

void pintaInicio() {
  gfx->fillScreen(NEGRO);
  aro(GRIS);
  centra("ECG", 88, 5, BLANCO);
  centra("AD8232", 136, 2, GRIS);
}

void pintaEspera() {
  gfx->fillScreen(NEGRO);
  aro(AZUL);
  centra("LISTO", 78, 4, AZUL);
  centra("COLOCA LOS", 130, 1, GRIS);
  centra("ELECTRODOS", 144, 1, GRIS);
  // Linea plana: dice "conectado pero sin senal" mejor que un hueco.
  gfx->drawFastHLine(BANDA_X0, 175, BANDA_W, GRIS);
}

void pintaMideFijo() {
  gfx->fillScreen(NEGRO);
  aro(VERDE);
  centra("lpm", 110, 2, GRIS);
}

void tareaPantalla(void *)
{
  VERDE  = gfx->color565(0, 235, 120);
  AZUL   = gfx->color565(0, 145, 255);
  BLANCO = gfx->color565(245, 245, 245);
  GRIS   = gfx->color565(130, 120, 116);

  pintaInicio();
  rgbLedWrite(PIN_LED_PLACA, 40, 40, 40);   // gris: hay vida
  vTaskDelay(pdMS_TO_TICKS(1200));          // que se alcance a leer

  Vista vista = V_INICIO;
  int      ult_bpm = -1;
  uint32_t ult_rr_ms = 0;

  int   col = 0, dec = 0, pico_dec = 0;
  int   y_prev = BANDA_MED;
  float escala = 120.0f;
  uint32_t ult_led = 0xFFFFFFFF;

  TickType_t t = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&t, pdMS_TO_TICKS(40));    // 25 cuadros por segundo

    // LED de la placa, en paralelo al aro de la pantalla.  Solo se
    // escribe cuando el color cambia: rgbLedWrite habla por RMT y no
    // tiene sentido repetirlo 25 veces por segundo.
    uint32_t color;
    if (!g_contacto)                        color = 0x000030;   // azul
    else if (millis() - g_latido < 90)      color = 0x3C3C3C;   // destello
    else                                    color = 0x003000;   // verde
    if (color != ult_led) {
      ult_led = color;
      rgbLedWrite(PIN_LED_PLACA, (color >> 16) & 0xFF,
                                 (color >> 8) & 0xFF, color & 0xFF);
    }

    Vista nueva = g_contacto ? V_MIDE : V_ESPERA;
    if (nueva != vista) {
      vista = nueva;
      if (vista == V_ESPERA) pintaEspera();
      else {
        pintaMideFijo();
        col = 0; dec = 0; pico_dec = 0; y_prev = BANDA_MED;
      }
      ult_bpm = -1; ult_rr_ms = 0;
    }

    // Vaciar siempre el buffer, se dibuje o no: si no, al recuperar
    // el contacto se dibujarian de golpe los segundos acumulados.
    uint32_t w = w_idx;
    if (w - r_idx > NBUF) r_idx = w - NBUF;
    while (r_idx != w) {
      int16_t s = rbuf[r_idx % NBUF];
      r_idx++;
      if (vista != V_MIDE) continue;

      // Decimar quedandose con la muestra de mayor amplitud del grupo:
      // si se tomara la primera, el QRS se perderia en muchos latidos.
      if (abs(s) > abs(pico_dec)) pico_dec = s;
      if (++dec < DECIMA) continue;
      dec = 0;
      int16_t val = pico_dec;
      pico_dec = 0;

      float a = fabsf((float)val);
      escala = (a > escala) ? a : (escala * 0.999f);
      if (escala < 40.0f) escala = 40.0f;

      int x = BANDA_X0 + col;
      int y = BANDA_MED - (int)((float)val * (BANDA_H / 2 - 4) / escala);
      if (y < BANDA_Y0) y = BANDA_Y0;
      if (y > BANDA_Y1) y = BANDA_Y1;

      int borra = min(7, BANDA_X1 - x);
      if (borra > 0) gfx->fillRect(x, BANDA_Y0, borra, BANDA_H, NEGRO);

      if (col == 0) y_prev = y;
      else          gfx->drawLine(x - 1, y_prev, x, y, VERDE);
      y_prev = y;

      if (++col >= BANDA_W) col = 0;
    }

    if (vista != V_MIDE) continue;

    int bpm = (int)(g_bpm + 0.5f);
    if (bpm != ult_bpm) {
      ult_bpm = bpm;
      gfx->fillRect(40, 54, 160, 50, NEGRO);
      char s[8];
      if (bpm > 0 && bpm < 300) snprintf(s, sizeof s, "%d", bpm);
      else                      snprintf(s, sizeof s, "--");
      centra(s, 56, 6, BLANCO);
    }

    uint32_t rr_ms = (uint32_t)(g_rr * 1000.0f);
    if (rr_ms / 10 != ult_rr_ms / 10) {
      ult_rr_ms = rr_ms;
      gfx->fillRect(50, 206, 140, 14, NEGRO);
      char s[24];
      if (rr_ms > 0) snprintf(s, sizeof s, "R-R %lu ms", (unsigned long)rr_ms);
      else           snprintf(s, sizeof s, "midiendo...");
      centra(s, 208, 1, GRIS);
    }
  }
}

// ---------------------------------------------------------------- setup
void setup()
{
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Sin esto, si nadie tiene abierto el puerto la escritura se bloquea
  // y el muestreo se detiene con ella.
  Serial.setTxTimeoutMs(0);
#endif

  pinMode(PIN_LOA, INPUT);
  pinMode(PIN_LOB, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ECG, ADC_11db);
  disenaFiltros();

  // 10 MHz y no 40: los cables de protoboard son largos y sin
  // blindaje, y mas rapido el bus corrompe datos (se ve como lineas).
  if (!gfx->begin(10000000)) {
    Serial.println("# ERROR: la pantalla GC9A01 no responde");
  }

  xTaskCreatePinnedToCore(tareaPantalla, "pantalla", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(tareaMuestreo, "muestreo", 4096, NULL, 5, NULL, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
