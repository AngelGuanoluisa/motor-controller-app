/*
 * Control de motor DC con ESP32 + BTS7960 (IBT-2) por BLE y Monitor Serie
 * ----------------------------------------------------------------------
 * Conexiones (ESP32 -> BTS7960):
 *   GPIO25 -> RPWM      (giro "adelante" / CW)
 *   GPIO26 -> LPWM      (giro "atrás" / CCW)
 *   GPIO27 -> R_EN
 *   GPIO14 -> L_EN
 *   GPIO34 -> R_IS      (solo entrada, ADC1)
 *   GPIO35 -> L_IS      (solo entrada, ADC1)
 *   3V3    -> VCC lógica del módulo
 *   GND    -> GND (común con la fuente del motor)
 *
 * Corriente: I_motor = (V_IS / R_IS) * K_ILIS
 *   El IBT-2 trae R_IS = 1 kΩ. El módulo tiene DOS chips (uno por lado) y cada
 *   uno está calibrado por separado: ADELANTE -> R_IS / K_ILIS_R,
 *   ATRAS -> L_IS / K_ILIS_L. Solo el chip del lado activo mide la corriente.
 *
 * Protección: si la corriente supera el límite (1.05 A por defecto,
 * configurable con SET_ILIM:<mA> o desde la app) se activa el paro de
 * emergencia y el motor se detiene.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ==========================================
// PINES BTS7960
// ==========================================
const int PIN_RPWM = 25;
const int PIN_LPWM = 26;
const int PIN_R_EN = 27;
const int PIN_L_EN = 14;
const int PIN_R_IS = 34;
const int PIN_L_IS = 35;

// ==========================================
// SENSADO DE CORRIENTE (valores calibrados)
// ==========================================
const float K_ILIS_R    = 566.0f;   // Relación I_carga / I_IS, chip lado R (adelante)
const float K_ILIS_L    = 793.5f;   // Relación I_carga / I_IS, chip lado L (atrás)
const float R_IS_OHMS   = 1000.0f;  // Resistencia en el pin IS (la del IBT-2)
const int   ADC_SAMPLES = 32;       // Muestras a promediar por lectura

// ==========================================
// GESTOR DE PARÁMETROS EN MEMORIA FLASH
// ==========================================
Preferences prefs;

// Parámetros configurables con valores por defecto
int pwmFreq = 20000;          // Frecuencia PWM en Hz (100 Hz a 40000 Hz)
int pwmResolution = 8;        // Resolución (8 bits: 0-255)
int speedMin = 0;             // Límite mínimo de velocidad
int speedMax = 255;           // Límite máximo de velocidad
int rampTimeMs = 500;         // Tiempo de rampa de aceleración/deceleración en ms (0 = instantáneo)
bool invertDirection = false; // Invertir lógica de giro (CW <-> CCW)

// Protección por corriente
int limiteCorriente_mA = 1050; // Límite de corriente (0 = desactivado)
int tiempoLimiteMs = 150;      // Tiempo continuo por encima del límite para disparar
int tiempoBlankingMs = 500;    // Tiempo ignorado tras arrancar / cambiar de sentido / acelerar

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
char ladoActivo = '-';        // 'R' = RPWM activo, 'L' = LPWM activo, '-' = ninguno

unsigned long lastRampUpdate = 0;

// Estado de la medición de corriente
float offsetR_mV = 0;
float offsetL_mV = 0;
float corriente_mA = 0;       // Última lectura del lado activo
float corrientePico_mA = 0;   // Pico desde el último START
unsigned long ultimaLecturaCorriente = 0;
unsigned long ultimoEnvioCorriente = 0;
unsigned long ultimoLogCorriente = 0;
unsigned long inicioMovimiento = 0; // Para ignorar el pico de arranque
unsigned long inicioExceso = 0;     // 0 = la corriente no está por encima del límite
bool logCorrienteSerie = true;

const unsigned long LECTURA_CORRIENTE_MS = 50;
const unsigned long ENVIO_CORRIENTE_MS = 250;

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
void calibrarOffset();
void enviarCorrienteBLE();
void imprimirCorriente();

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
  limiteCorriente_mA = prefs.getInt("iLimBts", 1050);
  tiempoLimiteMs = prefs.getInt("tLimite", 150);
  tiempoBlankingMs = prefs.getInt("tBlank", 500);
  prefs.end();

  Serial.println("[CONFIG] Parametros cargados de memoria Flash (NVS):");
  Serial.printf(" - Frecuencia: %d Hz\n", pwmFreq);
  Serial.printf(" - Limites Velocidad: Min=%d, Max=%d\n", speedMin, speedMax);
  Serial.printf(" - Rampa de aceleracion: %d ms\n", rampTimeMs);
  Serial.printf(" - Inversion de sentido: %s\n", invertDirection ? "SI" : "NO");
  Serial.printf(" - Limite de corriente: %d mA durante %d ms (se ignoran %d ms al arrancar)\n",
                limiteCorriente_mA, tiempoLimiteMs, tiempoBlankingMs);
}

void guardarConfiguracion() {
  prefs.begin("motor_cfg", false);
  prefs.putInt("freq", pwmFreq);
  prefs.putInt("res", pwmResolution);
  prefs.putInt("spdMin", speedMin);
  prefs.putInt("spdMax", speedMax);
  prefs.putInt("ramp", rampTimeMs);
  prefs.putBool("invDir", invertDirection);
  prefs.putInt("iLimBts", limiteCorriente_mA);
  prefs.putInt("tLimite", tiempoLimiteMs);
  prefs.putInt("tBlank", tiempoBlankingMs);
  prefs.end();
  Serial.println("[CONFIG] Parametros guardados permanentemente en Flash.");
}

void aplicarMotor(int pwmR, int pwmL) {
  setPwm(PIN_RPWM, 0, pwmR);
  setPwm(PIN_LPWM, 1, pwmL);
  ladoActivo = pwmR > 0 ? 'R' : (pwmL > 0 ? 'L' : '-');
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_R_EN, OUTPUT);
  pinMode(PIN_L_EN, OUTPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_R_IS, ADC_11db);  // rango ~0..3.1 V
  analogSetPinAttenuation(PIN_L_IS, ADC_11db);

  cargarConfiguracion();
  reconfigurarPWM();
  aplicarMotor(0, 0);
  calibrarOffset();   // Con el motor detenido
  actualizarSalida();

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

  // Medición de corriente y protección
  tareaCorriente();

  // Comandos Serial (PC)
  if (Serial.available() > 0) {
    String entrada = Serial.readStringUntil('\n');
    entrada.trim();
    if (entrada.length() > 0) procesarComando(entrada);
  }
}

// ==========================================
// CORRIENTE
// ==========================================
float leerMilliVolts(int pin) {
  uint32_t suma = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    suma += analogReadMilliVolts(pin);   // usa la calibración de fábrica del ADC
  }
  return (float)suma / ADC_SAMPLES;
}

float mVaAmperes(float mV, float k) {
  if (mV < 0) mV = 0;
  return (mV / 1000.0f) / R_IS_OHMS * k;
}

void leerCorriente(float &iR, float &iL, float &mvR, float &mvL) {
  mvR = leerMilliVolts(PIN_R_IS) - offsetR_mV;
  mvL = leerMilliVolts(PIN_L_IS) - offsetL_mV;
  iR = mVaAmperes(mvR, K_ILIS_R);
  iL = mVaAmperes(mvL, K_ILIS_L);
}

// Solo el chip del lado activo mide la corriente; el otro se ignora
float corrienteActiva(float iR, float iL) {
  if (ladoActivo == 'R') return iR;
  if (ladoActivo == 'L') return iL;
  return iR + iL;
}

void calibrarOffset() {
  // Igual que en la prueba de calibración: motor detenido y puentes deshabilitados
  aplicarMotor(0, 0);
  digitalWrite(PIN_R_EN, LOW);
  digitalWrite(PIN_L_EN, LOW);
  delay(200);
  offsetR_mV = leerMilliVolts(PIN_R_IS);
  offsetL_mV = leerMilliVolts(PIN_L_IS);
  Serial.printf("[CAL] Offset calibrado: R=%.1f mV, L=%.1f mV\n", offsetR_mV, offsetL_mV);
}

void tareaCorriente() {
  unsigned long ahora = millis();

  bool motorActivo = !paroEmergencia && velocidadActual > 0;
  if (!motorActivo) inicioMovimiento = ahora; // El tiempo ignorado cuenta desde que arranca

  if (ahora - ultimaLecturaCorriente >= LECTURA_CORRIENTE_MS) {
    ultimaLecturaCorriente = ahora;

    float iR, iL, mvR, mvL;
    leerCorriente(iR, iL, mvR, mvL);
    corriente_mA = corrienteActiva(iR, iL) * 1000.0f;
    if (motorActivo && corriente_mA > corrientePico_mA) corrientePico_mA = corriente_mA;

    // Protección: límite de corriente sostenido durante tiempoLimiteMs,
    // ignorando el pico de arranque (tiempoBlankingMs).
    bool enBlanking = ahora - inicioMovimiento < (unsigned long)tiempoBlankingMs;
    if (limiteCorriente_mA > 0 && motorActivo && !enBlanking && corriente_mA > limiteCorriente_mA) {
      if (inicioExceso == 0) inicioExceso = ahora;
      if (ahora - inicioExceso >= (unsigned long)tiempoLimiteMs) {
        inicioExceso = 0;
        float medida = corriente_mA;
        char lado = ladoActivo;
        procesarComando("E");
        Serial.printf("!! LIMITE DE CORRIENTE: %.0f mA > %d mA (lado %c). Motor detenido.\n",
                      medida, limiteCorriente_mA, lado);
        Serial.printf("   Para continuar: RESET_E, DIR:%s y START.\n", sentidoActual == HORARIO ? "CCW" : "CW");
        enviarNotificacionBLE("ALERT:GRIP");
      }
    } else {
      inicioExceso = 0;
    }
  }

  if (deviceConnected && ahora - ultimoEnvioCorriente >= ENVIO_CORRIENTE_MS) {
    ultimoEnvioCorriente = ahora;
    enviarCorrienteBLE();
  }
  if (logCorrienteSerie && enMarcha && ahora - ultimoLogCorriente >= 1000) {
    ultimoLogCorriente = ahora;
    imprimirCorriente();
  }
}

void enviarCorrienteBLE() {
  // CUR:<mA actual>,<mA pico>,<lado R|L|->,<límite mA>
  String curStr = "CUR:" + String((int)corriente_mA) + "," +
                  String((int)corrientePico_mA) + "," +
                  String(ladoActivo) + "," +
                  String(limiteCorriente_mA);
  enviarNotificacionBLE(curStr);
}

void imprimirCorriente() {
  float iR, iL, mvR, mvL;
  leerCorriente(iR, iL, mvR, mvL);
  Serial.printf("[CORRIENTE] Lado %c PWM=%3d | R_IS=%6.1f mV (%.2f A) | L_IS=%6.1f mV (%.2f A) | "
                "I=%.2f A | pico=%.2f A | limite=%.2f A\n",
                ladoActivo, velocidadActual, mvR, iR, mvL, iL,
                corriente_mA / 1000.0f, corrientePico_mA / 1000.0f, limiteCorriente_mA / 1000.0f);
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
    aplicarMotor(0, 0);
    digitalWrite(PIN_R_EN, LOW);  // Puentes deshabilitados -> rueda libre
    digitalWrite(PIN_L_EN, LOW);
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

// Cambio de sentido: si el motor está girando, frenar primero para evitar picos
void cambiarSentido(Sentido nuevo) {
  if (nuevo == sentidoActual) return;
  if (velocidadActual > 0) {
    aplicarMotor(0, 0);   // EN activos + ambos lados bajos -> freno
    delay(300);
    velocidadActual = (rampTimeMs <= 0 && enMarcha) ? velocidadObjetivo : 0;
  }
  sentidoActual = nuevo;
  inicioMovimiento = millis();
  corrientePico_mA = 0;
  actualizarSalida();
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
    paroEmergencia = false;
    actualizarSalida();
    Serial.println("[OK] Emergencia restablecida.");
    imprimirEstado();
    return;
  }

  // Durante la emergencia solo se bloquean los comandos que mueven el motor.
  // Consultas y configuración (GET_CFG, STATUS, SET_*, CAL_I0...) siguen
  // funcionando: la app envía GET_CFG al conectarse.
  bool esComandoMovimiento = cmd == "START" || cmd.startsWith("DIR:") || cmd.startsWith("SPD:");
  if (paroEmergencia && esComandoMovimiento) {
    Serial.println("[ERROR] Paro de emergencia activo. Envia RESET_E para restablecer.");
    return;
  }

  if (cmd == "START") {
    enMarcha = true;
    corrientePico_mA = 0; // El pico se mide por cada marcha
    if (rampTimeMs <= 0) velocidadActual = velocidadObjetivo;
    actualizarSalida();
    Serial.println("[OK] Marcha iniciada.");
  } else if (cmd == "STOP") {
    enMarcha = false;
    if (rampTimeMs <= 0) velocidadActual = 0;
    actualizarSalida();
    Serial.println("[OK] Detenido.");
  } else if (cmd == "DIR:CW") {
    cambiarSentido(HORARIO);
  } else if (cmd == "DIR:CCW") {
    cambiarSentido(ANTIHORARIO);
  } else if (cmd.startsWith("SPD:")) {
    int valor = cmd.substring(4).toInt();
    if (valor > velocidadObjetivo) inicioMovimiento = millis(); // Acelerar también genera un pico
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
  else if (cmd.startsWith("SET_ILIM:")) {
    int i = cmd.substring(9).toInt();
    if (i >= 0 && i <= 43000) {
      limiteCorriente_mA = i;
      guardarConfiguracion();
      if (i == 0) Serial.println("[CFG] Limite de corriente DESACTIVADO");
      else Serial.printf("[CFG] Limite de corriente: %d mA\n", limiteCorriente_mA);
      enviarCorrienteBLE();
    }
  } else if (cmd.startsWith("SET_TLIM:")) {
    int t = cmd.substring(9).toInt();
    if (t >= 0 && t <= 5000) {
      tiempoLimiteMs = t;
      guardarConfiguracion();
      Serial.printf("[CFG] Tiempo por encima del limite para detener: %d ms\n", tiempoLimiteMs);
    }
  } else if (cmd.startsWith("SET_TBLANK:")) {
    int t = cmd.substring(11).toInt();
    if (t >= 0 && t <= 10000) {
      tiempoBlankingMs = t;
      guardarConfiguracion();
      Serial.printf("[CFG] Tiempo ignorado tras arrancar: %d ms\n", tiempoBlankingMs);
    }
  } else if (cmd == "CUR" || cmd == "GET_CUR") {
    imprimirCorriente();
    enviarCorrienteBLE();
  } else if (cmd == "CAL_I0") {
    if (enMarcha || velocidadActual > 0) {
      Serial.println("[ERROR] Deten el motor (STOP) antes de calibrar el offset.");
    } else {
      calibrarOffset();
      actualizarSalida();
    }
  } else if (cmd == "CUR_LOG:1" || cmd == "CUR_LOG:0") {
    logCorrienteSerie = (cmd == "CUR_LOG:1");
  }

  else if (cmd == "GET_CFG") {
    enviarConfiguracionBLE();
    enviarCorrienteBLE();
  } else if (cmd == "STATUS") {
    imprimirEstado();
  } else {
    Serial.printf("[ERROR] Comando desconocido: %s\n", cmd.c_str());
  }
}

void imprimirEstado() {
  Serial.print("--- ESTADO: [");
  if (paroEmergencia) Serial.print("PARO EMERGENCIA");
  else if (enMarcha)  Serial.print("EN MARCHA");
  else                Serial.print("DETENIDO");
  Serial.print("] | Sentido: ");
  Serial.print(sentidoActual == HORARIO ? "CW" : "CCW");
  Serial.printf(" | Objetivo: %d | Real PWM: %d | Freq: %d Hz | Corriente: %.2f A | Limite: %.2f A ---\n",
                velocidadObjetivo, velocidadActual, pwmFreq, corriente_mA / 1000.0f, limiteCorriente_mA / 1000.0f);
}
