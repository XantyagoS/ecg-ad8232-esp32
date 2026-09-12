/*
  Paso 1, version minima: muestrea el AD8232 a 250 Hz e imprime UNA
  linea cada 5 segundos.  Sin LED y sin pruebas de pines, para aislar
  que parte estaba colgando la placa.

    AD8232 OUTPUT -> GPIO4     LO+ -> GPIO5     LO- -> GPIO6
    AD8232 3.3V   -> 3V3       GND -> GND       SDN -> 3V3
*/

const int PIN_ECG = 4;
const int PIN_LOP = 5;
const int PIN_LON = 6;

const unsigned long PERIODO_US = 4000UL;   // 250 Hz
const unsigned long INFORME_MS = 5000UL;

unsigned long t_prev = 0, t_informe = 0;
long suma = 0;
int  n = 0, v_min = 4095, v_max = 0, n_despegado = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);                 // margen para que el host abra el puerto
  pinMode(PIN_LOP, INPUT);
  pinMode(PIN_LON, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ECG, ADC_11db);
  Serial.println();
  Serial.println("ECG AD8232 + ESP32-S3 - informe cada 5 s");
  Serial.println("----------------------------------------");
  t_prev = micros();
  t_informe = millis();
}

void loop() {
  while ((long)(micros() - t_prev) < (long)PERIODO_US) { /* espera */ }
  t_prev += PERIODO_US;
  if ((long)(micros() - t_prev) > (long)(4 * PERIODO_US)) t_prev = micros();

  bool despegado = (digitalRead(PIN_LOP) == HIGH || digitalRead(PIN_LON) == HIGH);
  int v = analogRead(PIN_ECG);

  n++;
  suma += v;
  if (v < v_min) v_min = v;
  if (v > v_max) v_max = v;
  if (despegado) n_despegado++;

  if (millis() - t_informe >= INFORME_MS) {
    t_informe += INFORME_MS;
    int med = n ? (int)(suma / n) : 0;
    Serial.printf("[%4lu s] %-12s  ADC min=%4d med=%4d max=%4d  (%d muestras)\n",
                  millis() / 1000,
                  n_despegado > n / 2 ? "sin contacto" : "con contacto",
                  v_min, med, v_max, n);
    suma = 0; n = 0; v_min = 4095; v_max = 0; n_despegado = 0;
  }
}
