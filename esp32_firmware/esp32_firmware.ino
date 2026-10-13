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
  prefs.end();

  Serial.println("[CONFIG] Parametros cargados de memoria Flash (NVS):");
  Serial.printf(" - Frecuencia: %d Hz\n", pwmFreq);
  Serial.printf(" - Limites Velocidad: Min=%d, Max=%d\n", speedMin, speedMax);
  Serial.printf(" - Rampa de aceleracion: %d ms\n", rampTimeMs);
  Serial.printf(" - Inversion de sentido: %s\n", invertDirection ? "SI" : "NO");
}

void guardarConfiguracion() {
  prefs.begin("motor_cfg", false);
  prefs.putInt("freq", pwmFreq);
  prefs.putInt("res", pwmResolution);
  prefs.putInt("spdMin", speedMin);
  prefs.putInt("spdMax", speedMax);
  prefs.putInt("ramp", rampTimeMs);
  prefs.putBool("invDir", invertDirection);
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

  cargarConfiguracion();
  reconfigurarPWM();
  aplicarMotor(0, 0);

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

    //Paro por desconexión
    Serial.println("[!] Conexión perdida. Deteniendo motor por seguridad.");
    enMarcha = false;
    velocidadActual = 0; // Detener instantáneamente (sin rampa)
    paroEmergencia = true; // Opcional: bloquea el sistema hasta recibir un RESET_E
    actualizarSalida();
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

  // Comandos Serial (PC)
  if (Serial.available() > 0) {
    String entrada = Serial.readStringUntil('\n');
    entrada.trim();
    if (entrada.length() > 0) procesarComando(entrada);
  }
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
    paroEmergencia = false;
    actualizarSalida();
    Serial.println("[OK] Emergencia restablecida.");
    imprimirEstado();
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
  Serial.printf(" | Objetivo: %d | Real PWM: %d | Freq: %d Hz ---\n", velocidadObjetivo, velocidadActual, pwmFreq);
}
