/*
  ECG con AD8232 + Arduino UNO/Nano
  Salida: valor crudo del ADC por Serial, listo para Serial Plotter
          o para el script python (plot_ecg.py).

  Conexiones:
    AD8232 GND    -> Arduino GND
    AD8232 3.3V   -> Arduino 3.3V     (NO 5V)
    AD8232 OUTPUT -> A0
    AD8232 LO+    -> D11
    AD8232 LO-    -> D10
    AD8232 SDN    -> sin conectar (o 3.3V para mantenerlo activo)
    (opcional) Arduino 3.3V -> AREF, y descomentar analogReference(EXTERNAL)

  IMPORTANTE: alimentar con batería/powerbank cuando haya electrodos
  en una persona. Nunca con el portátil enchufado a la red.
*/

const uint8_t PIN_ECG = A0;
const uint8_t PIN_LOP = 11;   // Leads-Off +
const uint8_t PIN_LON = 10;   // Leads-Off -

// 250 Hz de muestreo: suficiente para ver P, QRS y T con buena forma.
const unsigned long PERIODO_US = 4000UL;   // 1/250 s
unsigned long t_prev = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LOP, INPUT);
  pinMode(PIN_LON, INPUT);
  // analogReference(EXTERNAL); // SOLO si conectaste AREF a 3.3V. Sin ese
                                // cable puentea, no lo actives (daña el ADC).
  t_prev = micros();
}

void loop() {
  // muestreo a intervalo fijo (no usar delay: se descuadra el tiempo)
  while ((unsigned long)(micros() - t_prev) < PERIODO_US) { /* espera */ }
  t_prev += PERIODO_US;

  if (digitalRead(PIN_LOP) == HIGH || digitalRead(PIN_LON) == HIGH) {
    Serial.println(0);          // electrodo despegado
  } else {
    Serial.println(analogRead(PIN_ECG));
  }
}
