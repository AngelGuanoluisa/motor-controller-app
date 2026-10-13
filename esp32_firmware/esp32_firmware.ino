#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ==========================================
// PINES BTS7960
// ==========================================
const int PIN_RPWM = 32;
const int PIN_LPWM = 33;
const int PIN_R_EN = 25;
const int PIN_L_EN = 26;

// Sensado de corriente del BTS7960 (pines R_IS y L_IS del módulo IBT-2).
// Deben ir a pines ADC1 (el ADC2 no funciona con Bluetooth activo).
// GPIO 34 y 35 son solo de entrada, ideales para esto.
const int PIN_R_IS = 34;
const int PIN_L_IS = 35;

// ==========================================
// GESTOR DE PARÁMETROS EN MEMORIA FLASH
// ==========================================
Preferences prefs;

// Parámetros configurables con valores por defecto
int pwmFreq = 20000;         // Frecuencia PWM en Hz (100 Hz a 40000 Hz)
int pwmResolution = 8;       // Resolución (8 bits: 0-255)
int speedMin = 0;            // Límite mínimo de velocidad
int speedMax = 255;          // Límite máximo de velocidad
int rampTimeMs = 500;        // Tiempo de rampa de aceleración/deceleración en ms (0 = instantáneo)
bool invertDirection = false; // Invertir lógica de giro (CW <-> CCW)

// ==========================================
// MEDICIÓN / ESTIMACIÓN DE CORRIENTE
// ==========================================
// El BTS7960 entrega por IS una corriente proporcional a la del motor:
//     I_IS = I_motor / kILIS        (kILIS típico = 8500)
// Con una resistencia R_IS de IS a GND se obtiene una tensión:
//     V_IS = I_motor * R_IS / kILIS   ->   I_motor = V_IS * kILIS / R_IS
// Con R_IS = 680 ohm: 1 V equivale a 12.5 A; 3.1 V (máx. ADC) ~ 38 A.
// IMPORTANTE: ante un fallo el BTS7960 saca hasta ~4.5 mA por IS. Con 680 ohm
// eso da ~3.06 V (seguro para la ESP32); con 1 kOhm daría 4.5 V y dañaría el pin.
//
// Modo SENSOR: usa la lectura real de R_IS + L_IS.
// Modo ESTIMADO: sin sensor, aproxima I = corrienteNominal * (PWM / 255).
//   "corrienteNominal" se obtiene midiendo una vez con un multímetro/pinza
//   la corriente con el motor a PWM 255 y su carga habitual.
//
// Nota: con PWM, la corriente de la fuente es pulsante; el valor reportado
// es el PROMEDIO (lo que marcaría un multímetro en serie con la fuente).
bool modoSensorCorriente = true; // true = SENSOR (IS), false = ESTIMADO
float kILIS = 8500.0;            // Relación de sensado del BTS7960 (datasheet: 8500 típ.)
float resistenciaIS = 680.0;     // Resistencia de IS a GND en ohmios
int offsetR_mV = 0;              // Tensión en R_IS con motor detenido (calibración)
int offsetL_mV = 0;              // Tensión en L_IS con motor detenido (calibración)
int corrienteNominal_mA = 2000;  // Para modo ESTIMADO: corriente a PWM 255
int limiteCorriente_mA = 0;      // Protección de sobrecorriente (0 = desactivada)

float corrienteFiltrada_mA = 0;  // Valor promedio actual (filtrado)
float corrientePico_mA = 0;      // Pico desde el último reset (CUR_RESET)
float energia_mAh = 0;           // Carga consumida acumulada (mAh)

// LÍMITE DE CONSUMO (mAh): al alcanzarlo se activa el paro de emergencia y el
// motor queda BLOQUEADO (RESET_E no lo libera, ni siquiera tras reiniciar la
// ESP32). Para desbloquear hay que reiniciar el contador con CUR_RESET.
int limiteConsumo_mAh = 750;     // 0 = sin límite
bool bloqueoConsumo = false;
float energiaGuardada_mAh = 0;   // Último valor escrito en flash
unsigned long ultimoGuardadoConsumo = 0;
const unsigned long GUARDADO_CONSUMO_MS = 30000; // Guardar mAh cada 30 s como máximo
unsigned long proximaMuestraUs = 0;
unsigned long ultimoCalculoCorriente = 0;
unsigned long ultimoEnvioCorriente = 0;
unsigned long ultimoLogCorriente = 0;
long acumR_mV = 0, acumL_mV = 0;
int numMuestras = 0;
int muestrasSaturadas = 0;
int ventanasSobrecorriente = 0;
bool logCorrienteSerie = true;

const int ADC_SATURACION_MV = 3000;          // Cerca del máximo del ADC del ESP32
const unsigned long VENTANA_CORRIENTE_MS = 100;
const unsigned long ENVIO_CORRIENTE_MS = 250;

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 0, 0)
const int PWM_CHAN_R = 0;
const int PWM_CHAN_L = 1;
#endif

// ==========================================
// UUIDs BLE NORDIC UART
// ==========================================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Estado del Motor
enum Sentido { HORARIO = 1, ANTIHORARIO = -1 };
int velocidadActual = 0;      // Velocidad física actual aplicada al PWM
int velocidadObjetivo = 150;  // Velocidad seleccionada
Sentido sentidoActual = HORARIO;
bool enMarcha = false;
bool paroEmergencia = false;

unsigned long lastRampUpdate = 0;

void reconfigurarPWM();
void cargarConfiguracion();
void guardarConfiguracion();
void procesarComando(String cmd);
void actualizarSalida();
void aplicarMotor(int pwmR, int pwmL);
void setPwm(int pin, int canal, int valor);
void enviarNotificacionBLE(String mensaje);
void enviarConfiguracionBLE();
void imprimirEstado();
void tareaCorriente();
void calibrarCeroCorriente();
void enviarCorrienteBLE();
void imprimirCorriente();
void guardarConsumo();

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("[BLE] Celular conectado.");
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("[BLE] Celular desconectado.");
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
        String rxValue = pCharacteristic->getValue().c_str();
      #else
        std::string val = pCharacteristic->getValue();
        String rxValue = String(val.c_str());
      #endif
      
      rxValue.trim();
      if (rxValue.length() > 0) {
        Serial.print("[BLE Comando]: ");
        Serial.println(rxValue);
        procesarComando(rxValue);
      }
    }
};

void setPwm(int pin, int canal, int valor) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(pin, valor);
#else
  ledcWrite(canal, valor);
#endif
}

void reconfigurarPWM() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcDetach(PIN_RPWM);
  ledcDetach(PIN_LPWM);
  ledcAttach(PIN_RPWM, pwmFreq, pwmResolution);
  ledcAttach(PIN_LPWM, pwmFreq, pwmResolution);
#else
  ledcSetup(PWM_CHAN_R, pwmFreq, pwmResolution);
  ledcSetup(PWM_CHAN_L, pwmFreq, pwmResolution);
  ledcAttachPin(PIN_RPWM, PWM_CHAN_R);
  ledcAttachPin(PIN_LPWM, PWM_CHAN_L);
#endif
  Serial.printf("[PWM] Frecuencia reconfigurada a %d Hz (Resolucion: %d bits)\n", pwmFreq, pwmResolution);
}

void cargarConfiguracion() {
  prefs.begin("motor_cfg", false);
  pwmFreq = prefs.getInt("freq", 20000);
  pwmResolution = prefs.getInt("res", 8);
  speedMin = prefs.getInt("spdMin", 0);
  speedMax = prefs.getInt("spdMax", 255);
  rampTimeMs = prefs.getInt("ramp", 500);
  invertDirection = prefs.getBool("invDir", false);
  modoSensorCorriente = prefs.getBool("curSensor", true);
  kILIS = prefs.getFloat("kilis", 8500.0);
  resistenciaIS = prefs.getFloat("rIS", 680.0);
  offsetR_mV = prefs.getInt("offR", 0);
  offsetL_mV = prefs.getInt("offL", 0);
  corrienteNominal_mA = prefs.getInt("iNom", 2000);
  limiteCorriente_mA = prefs.getInt("iLim", 0);
  limiteConsumo_mAh = prefs.getInt("mahLim", 750);
  energia_mAh = prefs.getFloat("mah", 0);
  bloqueoConsumo = prefs.getBool("lockMah", false);
  energiaGuardada_mAh = energia_mAh;
  prefs.end();

  Serial.println("[CONFIG] Parametros cargados de memoria Flash (NVS):");
  Serial.printf(" - Frecuencia: %d Hz\n", pwmFreq);
  Serial.printf(" - Limites Velocidad: Min=%d, Max=%d\n", speedMin, speedMax);
  Serial.printf(" - Rampa de aceleracion: %d ms\n", rampTimeMs);
  Serial.printf(" - Inversion de sentido: %s\n", invertDirection ? "SI" : "NO");
  Serial.printf(" - Corriente: modo %s | kILIS=%.0f | R_IS=%.0f ohm | Offset R/L=%d/%d mV | I_nom=%d mA | Limite=%d mA\n",
                modoSensorCorriente ? "SENSOR" : "ESTIMADO", kILIS, resistenciaIS,
                offsetR_mV, offsetL_mV, corrienteNominal_mA, limiteCorriente_mA);
  Serial.printf(" - Consumo acumulado: %.1f mAh | Limite: %d mAh | Bloqueado: %s\n",
                energia_mAh, limiteConsumo_mAh, bloqueoConsumo ? "SI" : "NO");
}

void guardarConfiguracion() {
  prefs.begin("motor_cfg", false);
  prefs.putInt("freq", pwmFreq);
  prefs.putInt("res", pwmResolution);
  prefs.putInt("spdMin", speedMin);
  prefs.putInt("spdMax", speedMax);
  prefs.putInt("ramp", rampTimeMs);
  prefs.putBool("invDir", invertDirection);
  prefs.putBool("curSensor", modoSensorCorriente);
  prefs.putFloat("kilis", kILIS);
  prefs.putFloat("rIS", resistenciaIS);
  prefs.putInt("offR", offsetR_mV);
  prefs.putInt("offL", offsetL_mV);
  prefs.putInt("iNom", corrienteNominal_mA);
  prefs.putInt("iLim", limiteCorriente_mA);
  prefs.putInt("mahLim", limiteConsumo_mAh);
  prefs.end();
  Serial.println("[CONFIG] Parametros guardados permanentemente en Flash.");
}

void aplicarMotor(int pwmR, int pwmL) {
  setPwm(PIN_RPWM, 0, pwmR);
  setPwm(PIN_LPWM, 1, pwmL);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_R_EN, OUTPUT);
  pinMode(PIN_L_EN, OUTPUT);
  digitalWrite(PIN_R_EN, HIGH);
  digitalWrite(PIN_L_EN, HIGH);

  pinMode(PIN_R_IS, INPUT);
  pinMode(PIN_L_IS, INPUT);
  analogReadResolution(12); // Atenuación por defecto 11 dB: rango ~0-3.1 V

  cargarConfiguracion();
  reconfigurarPWM();
  aplicarMotor(0, 0);
  if (bloqueoConsumo) {
    paroEmergencia = true;
    actualizarSalida();
    Serial.println("[!] Motor BLOQUEADO por limite de consumo. Envia CUR_RESET para desbloquear.");
  }

  // Iniciar BLE
  BLEDevice::init("ESP32-MotorBTS");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("\n[SISTEMA LISTO] Anunciando BLE como 'ESP32-MotorBTS'...");
  imprimirEstado();
}

void loop() {
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    // Al conectar, enviamos los parámetros actuales a la app
    delay(200);
    enviarConfiguracionBLE();
  }

  // Rampa suave de aceleración/desaceleración
  if (!paroEmergencia) {
    int target = enMarcha ? constrain(velocidadObjetivo, speedMin, speedMax) : 0;
    if (velocidadActual != target) {
      if (rampTimeMs <= 0) {
        velocidadActual = target;
      } else {
        int stepDelay = rampTimeMs / (speedMax - speedMin > 0 ? (speedMax - speedMin) : 255);
        if (stepDelay < 2) stepDelay = 2;
        if (millis() - lastRampUpdate >= stepDelay) {
          lastRampUpdate = millis();
          if (velocidadActual < target) velocidadActual++;
          else if (velocidadActual > target) velocidadActual--;
        }
      }
      actualizarSalida();
    }
  }

  // Medición / estimación de corriente (no bloqueante)
  tareaCorriente();

  // Comandos Serial (PC)
  if (Serial.available() > 0) {
    String entrada = Serial.readStringUntil('\n');
    entrada.trim();
    if (entrada.length() > 0) procesarComando(entrada);
  }
}

// ==========================================
// CORRIENTE: muestreo, filtrado y protección
// ==========================================
float corrienteDesdeMilivoltios(float mV) {
  if (mV < 0) mV = 0;
  // I_motor[mA] = V_IS[mV] * kILIS / R_IS[ohm]
  return mV * kILIS / resistenciaIS;
}

void tareaCorriente() {
  unsigned long ahoraUs = micros();

  // 1) Tomar una muestra cada ~1 ms con un pequeño retardo aleatorio para no
  //    "sincronizarse" con el PWM (20 kHz es múltiplo exacto de 1 kHz) y así
  //    obtener un promedio real de la señal pulsante.
  if (modoSensorCorriente && (long)(ahoraUs - proximaMuestraUs) >= 0) {
    proximaMuestraUs = ahoraUs + 700 + random(0, 600);
    int mvR = analogReadMilliVolts(PIN_R_IS);
    int mvL = analogReadMilliVolts(PIN_L_IS);
    acumR_mV += mvR;
    acumL_mV += mvL;
    numMuestras++;
    // Si IS queda pegado arriba: sobrecorriente fuerte o fallo del driver
    // (el BTS7960 fuerza IS a su valor máximo ante sobretemperatura/corto).
    if (mvR >= ADC_SATURACION_MV || mvL >= ADC_SATURACION_MV) muestrasSaturadas++;
  }

  // 2) Cada 100 ms calcular el promedio de la ventana
  unsigned long ahora = millis();
  if (ahora - ultimoCalculoCorriente >= VENTANA_CORRIENTE_MS) {
    float dt_h = (ahora - ultimoCalculoCorriente) / 3600000.0;
    ultimoCalculoCorriente = ahora;

    float instantanea_mA = 0;
    bool saturado = false;
    if (modoSensorCorriente) {
      if (numMuestras > 0) {
        // Se suman ambos lados: el medio puente inactivo solo aporta su offset,
        // así no importa el sentido de giro.
        float mvR = (float)acumR_mV / numMuestras - offsetR_mV;
        float mvL = (float)acumL_mV / numMuestras - offsetL_mV;
        instantanea_mA = corrienteDesdeMilivoltios(mvR) + corrienteDesdeMilivoltios(mvL);
        saturado = muestrasSaturadas > numMuestras / 2;
      }
    } else {
      // Estimación sin sensor: proporcional al ciclo útil del PWM
      instantanea_mA = paroEmergencia ? 0 : corrienteNominal_mA * (velocidadActual / 255.0);
    }
    acumR_mV = acumL_mV = 0;
    numMuestras = 0;
    muestrasSaturadas = 0;

    // Filtro exponencial para suavizar la lectura
    corrienteFiltrada_mA = 0.7 * corrienteFiltrada_mA + 0.3 * instantanea_mA;
    if (corrienteFiltrada_mA < 5) corrienteFiltrada_mA = 0; // Ruido del ADC
    if (corrienteFiltrada_mA > corrientePico_mA) corrientePico_mA = corrienteFiltrada_mA;
    energia_mAh += corrienteFiltrada_mA * dt_h;

    // Límite de consumo acumulado (mAh) -> paro de emergencia con bloqueo
    if (limiteConsumo_mAh > 0 && !bloqueoConsumo && energia_mAh >= limiteConsumo_mAh) {
      bloqueoConsumo = true;
      Serial.printf("\n[!] LIMITE DE CONSUMO ALCANZADO: %.1f mAh (limite %d mAh). Motor BLOQUEADO.\n",
                    energia_mAh, limiteConsumo_mAh);
      procesarComando("E");
      guardarConsumo();
      enviarNotificacionBLE("ALERT:MAH_LIMIT");
    }
    // Guardar periódicamente el consumo para no perderlo si se corta la energía
    if (ahora - ultimoGuardadoConsumo >= GUARDADO_CONSUMO_MS && energia_mAh - energiaGuardada_mAh >= 0.5) {
      guardarConsumo();
    }

    // 3) Protección de sobrecorriente (solo si hay límite configurado).
    //    Se exigen 3 ventanas seguidas (300 ms) para ignorar el pico de arranque.
    if (limiteCorriente_mA > 0 && !paroEmergencia) {
      bool exceso = corrienteFiltrada_mA > limiteCorriente_mA || (modoSensorCorriente && saturado);
      ventanasSobrecorriente = exceso ? ventanasSobrecorriente + 1 : 0;
      if (ventanasSobrecorriente >= 3) {
        ventanasSobrecorriente = 0;
        Serial.printf("\n[!] SOBRECORRIENTE: %.0f mA (limite %d mA)%s\n", corrienteFiltrada_mA,
                      limiteCorriente_mA, saturado ? " - IS saturado / fallo del driver" : "");
        procesarComando("E");
        enviarNotificacionBLE("ALERT:OVERCURRENT");
      }
    }
  }

  // 4) Enviar a la app por BLE y mostrar por Serial
  if (deviceConnected && ahora - ultimoEnvioCorriente >= ENVIO_CORRIENTE_MS) {
    ultimoEnvioCorriente = ahora;
    enviarCorrienteBLE();
  }
  if (logCorrienteSerie && enMarcha && ahora - ultimoLogCorriente >= 1000) {
    ultimoLogCorriente = ahora;
    imprimirCorriente();
  }
}

void guardarConsumo() {
  ultimoGuardadoConsumo = millis();
  energiaGuardada_mAh = energia_mAh;
  prefs.begin("motor_cfg", false);
  prefs.putFloat("mah", energia_mAh);
  prefs.putBool("lockMah", bloqueoConsumo);
  prefs.end();
}

void calibrarCeroCorriente() {
  // Con el motor detenido, la tensión en IS es solo el offset del driver
  // (más el error del ADC). Se guarda para restarlo en cada medición.
  long sumaR = 0, sumaL = 0;
  const int N = 200;
  for (int i = 0; i < N; i++) {
    sumaR += analogReadMilliVolts(PIN_R_IS);
    sumaL += analogReadMilliVolts(PIN_L_IS);
    delay(2);
  }
  offsetR_mV = sumaR / N;
  offsetL_mV = sumaL / N;
  corrienteFiltrada_mA = 0;
  guardarConfiguracion();
  Serial.printf("[CAL] Cero de corriente: R_IS=%d mV, L_IS=%d mV (guardado)\n", offsetR_mV, offsetL_mV);
}

void enviarCorrienteBLE() {
  // CUR:<mA promedio>,<mA pico>,<mAh>,<S|E>,<límite mAh>,<bloqueado 0|1>
  // (S = sensor, E = estimado)
  String curStr = "CUR:" + String((int)corrienteFiltrada_mA) + "," +
                  String((int)corrientePico_mA) + "," +
                  String(energia_mAh, 1) + "," +
                  (modoSensorCorriente ? "S" : "E") + "," +
                  String(limiteConsumo_mAh) + "," +
                  (bloqueoConsumo ? "1" : "0");
  enviarNotificacionBLE(curStr);
}

void imprimirCorriente() {
  Serial.printf("[CORRIENTE %s] Promedio: %.2f A | Pico: %.2f A | Consumido: %.1f / %d mAh | PWM: %d\n",
                modoSensorCorriente ? "MEDIDA" : "ESTIMADA",
                corrienteFiltrada_mA / 1000.0, corrientePico_mA / 1000.0, energia_mAh,
                limiteConsumo_mAh, velocidadActual);
}

void enviarNotificacionBLE(String mensaje) {
  if (deviceConnected && pTxCharacteristic != NULL) {
    pTxCharacteristic->setValue(mensaje.c_str());
    pTxCharacteristic->notify();
  }
}

void enviarConfiguracionBLE() {
  // CFG:<freq>,<spdMin>,<spdMax>,<ramp>,<invDir>
  String cfgStr = "CFG:" + String(pwmFreq) + "," +
                  String(speedMin) + "," +
                  String(speedMax) + "," +
                  String(rampTimeMs) + "," +
                  String(invertDirection ? 1 : 0);
  enviarNotificacionBLE(cfgStr);
}

void actualizarSalida() {
  if (paroEmergencia) {
    digitalWrite(PIN_R_EN, LOW);
    digitalWrite(PIN_L_EN, LOW);
    aplicarMotor(0, 0);
    velocidadActual = 0;
  } else {
    digitalWrite(PIN_R_EN, HIGH);
    digitalWrite(PIN_L_EN, HIGH);

    if (velocidadActual == 0) {
      aplicarMotor(0, 0);
    } else {
      bool sentidoFinalHorario = (sentidoActual == HORARIO);
      if (invertDirection) sentidoFinalHorario = !sentidoFinalHorario;

      if (sentidoFinalHorario) {
        aplicarMotor(velocidadActual, 0);
      } else {
        aplicarMotor(0, velocidadActual);
      }
    }
  }

  // Telemetría: ST:<EMG|RUN|STOP>,<CW|CCW>,<spdObj>,<spdAct>
  String estadoStr = "ST:" + String(paroEmergencia ? "EMG" : (enMarcha ? "RUN" : "STOP")) +
                     "," + String(sentidoActual == HORARIO ? "CW" : "CCW") +
                     "," + String(velocidadObjetivo) +
                     "," + String(velocidadActual);
  enviarNotificacionBLE(estadoStr);
}

void procesarComando(String cmd) {
  cmd.toUpperCase();

  // COMANDOS DE OPERACIÓN
  if (cmd == "E" || cmd == "EMERGENCY") {
    paroEmergencia = true;
    enMarcha = false;
    velocidadActual = 0;
    actualizarSalida();
    Serial.println("\n[!] >>> PARO DE EMERGENCIA ACTIVADO <<<");
    return;
  }

  if (cmd == "RESET_E") {
    if (bloqueoConsumo) {
      Serial.println("[ERROR] Motor bloqueado por limite de consumo. Envia CUR_RESET para desbloquear.");
      enviarNotificacionBLE("ALERT:LOCKED");
      return;
    }
    paroEmergencia = false;
    actualizarSalida();
    Serial.println("[OK] Emergencia restablecida.");
    imprimirEstado();
    return;
  }

  // Comandos de consumo: se aceptan incluso durante la emergencia
  if (cmd == "CUR_RESET") {
    corrientePico_mA = 0;
    energia_mAh = 0;
    bool estabaBloqueado = bloqueoConsumo;
    bloqueoConsumo = false;
    guardarConsumo();
    Serial.println("[OK] Pico y mAh acumulados reiniciados.");
    if (estabaBloqueado) Serial.println("[OK] Bloqueo por consumo liberado. Envia RESET_E para rearmar.");
    enviarCorrienteBLE();
    return;
  }
  if (cmd.startsWith("SET_MAH_LIM:")) {
    int lim = cmd.substring(12).toInt();
    if (lim >= 0 && lim <= 100000) {
      limiteConsumo_mAh = lim;
      guardarConfiguracion();
      if (lim == 0) Serial.println("[CFG] Limite de consumo DESACTIVADO");
      else Serial.printf("[CFG] Limite de consumo: %d mAh\n", limiteConsumo_mAh);
      enviarCorrienteBLE();
    }
    return;
  }
  if (cmd == "CUR" || cmd == "GET_CUR") {
    imprimirCorriente();
    enviarCorrienteBLE();
    return;
  }

  if (paroEmergencia) {
    Serial.println("[ERROR] Sistema bloqueado por Emergencia.");
    return;
  }

  if (cmd == "START") {
    enMarcha = true;
    if (rampTimeMs <= 0) velocidadActual = velocidadObjetivo;
    actualizarSalida();
    Serial.println("[OK] Marcha iniciada.");
  } else if (cmd == "STOP") {
    enMarcha = false;
    if (rampTimeMs <= 0) velocidadActual = 0;
    actualizarSalida();
    guardarConsumo();
    Serial.println("[OK] Detenido.");
  } else if (cmd == "DIR:CW") {
    sentidoActual = HORARIO;
    actualizarSalida();
  } else if (cmd == "DIR:CCW") {
    sentidoActual = ANTIHORARIO;
    actualizarSalida();
  } else if (cmd.startsWith("SPD:")) {
    int valor = cmd.substring(4).toInt();
    velocidadObjetivo = constrain(valor, speedMin, speedMax);
    if (rampTimeMs <= 0 && enMarcha) velocidadActual = velocidadObjetivo;
    actualizarSalida();
  } 

  // COMANDOS DE CONFIGURACIÓN DINÁMICA
  else if (cmd.startsWith("SET_FREQ:")) {
    int f = cmd.substring(9).toInt();
    if (f >= 100 && f <= 40000) {
      pwmFreq = f;
      reconfigurarPWM();
      guardarConfiguracion();
      enviarConfiguracionBLE();
      Serial.printf("[CFG] Frecuencia guardada: %d Hz\n", pwmFreq);
    } else {
      Serial.println("[ERROR] Frecuencia valida entre 100 y 40000 Hz");
    }
  } else if (cmd.startsWith("SET_LIMITS:")) {
    // Formato: SET_LIMITS:<min>,<max>
    int sep = cmd.indexOf(',', 11);
    if (sep != -1) {
      int sMin = cmd.substring(11, sep).toInt();
      int sMax = cmd.substring(sep + 1).toInt();
      if (sMin >= 0 && sMax <= 255 && sMin < sMax) {
        speedMin = sMin;
        speedMax = sMax;
        guardarConfiguracion();
        enviarConfiguracionBLE();
        Serial.printf("[CFG] Limites guardados: Min=%d, Max=%d\n", speedMin, speedMax);
      }
    }
  } else if (cmd.startsWith("SET_RAMP:")) {
    int r = cmd.substring(9).toInt();
    if (r >= 0 && r <= 10000) {
      rampTimeMs = r;
      guardarConfiguracion();
      enviarConfiguracionBLE();
      Serial.printf("[CFG] Rampa guardada: %d ms\n", rampTimeMs);
    }
  } else if (cmd.startsWith("SET_INVERT:")) {
    int inv = cmd.substring(11).toInt();
    invertDirection = (inv == 1);
    guardarConfiguracion();
    actualizarSalida();
    enviarConfiguracionBLE();
    Serial.printf("[CFG] Inversion de sentido: %s\n", invertDirection ? "SI" : "NO");
  }

  // COMANDOS DE CORRIENTE
  else if (cmd == "CAL_I0") {
    if (enMarcha || velocidadActual > 0) {
      Serial.println("[ERROR] Detén el motor (STOP) antes de calibrar el cero.");
    } else {
      calibrarCeroCorriente();
    }
  } else if (cmd == "CUR_MODE:SENSOR" || cmd == "CUR_MODE:EST") {
    modoSensorCorriente = (cmd == "CUR_MODE:SENSOR");
    corrienteFiltrada_mA = 0;
    guardarConfiguracion();
    Serial.printf("[CFG] Modo de corriente: %s\n", modoSensorCorriente ? "SENSOR (IS)" : "ESTIMADO (PWM)");
  } else if (cmd.startsWith("SET_KILIS:")) {
    float k = cmd.substring(10).toFloat();
    if (k >= 1000 && k <= 20000) {
      kILIS = k;
      guardarConfiguracion();
      Serial.printf("[CFG] kILIS = %.0f\n", kILIS);
    } else {
      Serial.println("[ERROR] kILIS valido entre 1000 y 20000 (tipico 8500)");
    }
  } else if (cmd.startsWith("SET_RIS:")) {
    float r = cmd.substring(8).toFloat();
    if (r >= 100 && r <= 100000) {
      resistenciaIS = r;
      guardarConfiguracion();
      Serial.printf("[CFG] R_IS = %.0f ohm\n", resistenciaIS);
    } else {
      Serial.println("[ERROR] R_IS valida entre 100 y 100000 ohm");
    }
  } else if (cmd.startsWith("SET_INOM:")) {
    int i = cmd.substring(9).toInt();
    if (i > 0 && i <= 60000) {
      corrienteNominal_mA = i;
      guardarConfiguracion();
      Serial.printf("[CFG] Corriente nominal (modo estimado) = %d mA\n", corrienteNominal_mA);
    }
  } else if (cmd.startsWith("SET_ILIM:")) {
    int i = cmd.substring(9).toInt();
    if (i >= 0 && i <= 60000) {
      limiteCorriente_mA = i;
      guardarConfiguracion();
      if (i == 0) Serial.println("[CFG] Proteccion de sobrecorriente DESACTIVADA");
      else Serial.printf("[CFG] Proteccion de sobrecorriente: %d mA\n", limiteCorriente_mA);
    }
  } else if (cmd == "CUR_LOG:1" || cmd == "CUR_LOG:0") {
    logCorrienteSerie = (cmd == "CUR_LOG:1");
  } else if (cmd == "GET_CFG") {
    enviarConfiguracionBLE();
  } else if (cmd == "STATUS") {
    imprimirEstado();
  }
}

void imprimirEstado() {
  Serial.print("--- ESTADO: [");
  if (paroEmergencia) Serial.print("PARO EMERGENCIA");
  else if (enMarcha)  Serial.print("EN MARCHA");
  else                Serial.print("DETENIDO");
  Serial.print("] | Sentido: ");
  Serial.print(sentidoActual == HORARIO ? "CW" : "CCW");
  Serial.printf(" | Objetivo: %d | Real PWM: %d | Freq: %d Hz | Corriente: %.2f A ---\n",
                velocidadObjetivo, velocidadActual, pwmFreq, corrienteFiltrada_mA / 1000.0);
}
