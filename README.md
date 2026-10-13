# Motor Controller App — ESP32 + BTS7960 + Flutter (Android)

App móvil Android (Flutter) para controlar un motor DC de alta corriente con un driver
BTS7960 (IBT-2) y una placa ESP32, conectados por Bluetooth Low Energy (BLE).

---

## 📁 Estructura del Proyecto

```text
motor-controller-app/
├── .github/workflows/build_apk.yml   # Compila el APK automáticamente en GitHub Actions
├── android/                          # Proyecto Android nativo (Gradle, permisos BLE, MainActivity)
├── lib/main.dart                     # Código de la aplicación
├── test/widget_test.dart             # Prueba básica de la interfaz
├── esp32_firmware/esp32_firmware.ino # Firmware C++ para la ESP32 (Arduino IDE / PlatformIO)
└── pubspec.yaml                      # Dependencias Flutter
```

---

## 📥 Descargar el APK (sin instalar nada)

Cada push a `main`/`master` compila el APK en GitHub Actions:

1. Ve a la pestaña **Actions** del repositorio.
2. Abre la última ejecución de **Build APK** (también puedes lanzarla con *Run workflow*).
3. Descarga el artefacto **app-release** (es un `.zip` con `app-release.apk`).
4. Copia el APK al teléfono e instálalo (permite *instalar apps de origen desconocido*).

> El APK está firmado con la clave de depuración; sirve para instalar y probar,
> no para publicar en Google Play.

---

## ⚡ Conexión Eléctrica (ESP32 a BTS7960)

| Pin BTS7960 | Pin ESP32 | Descripción |
| :--- | :--- | :--- |
| **VCC** | 5V / VIN | Alimentación de la lógica del BTS7960 |
| **GND** | GND | Tierra compartida (¡común con fuente del motor!) |
| **R_EN** | GPIO 25 | Habilitar giro horario |
| **L_EN** | GPIO 26 | Habilitar giro antihorario |
| **RPWM** | GPIO 32 | Señal PWM giro horario (Right PWM) |
| **LPWM** | GPIO 33 | Señal PWM giro antihorario (Left PWM) |
| **B+ / B-** | Fuente Externa | Alimentación del motor (12V / 24V) |
| **M+ / M-** | Motor DC | Bornes del motor |

---

## 🚀 Paso 1: Cargar el Firmware en la ESP32

1. Abre `esp32_firmware/esp32_firmware.ino` con **Arduino IDE**.
2. Conecta la ESP32 por USB y selecciona placa y puerto COM.
3. Sube el programa.
4. Abre el **Monitor Serie** (`115200 baudios`, fin de línea *Nueva Línea (NL)*).
5. Verás: `[SISTEMA LISTO] Anunciando BLE como 'ESP32-MotorBTS'...`.
6. Comandos de prueba por consola:
   - `START` / `STOP`
   - `SPD:180` (rango 0 a 255)
   - `DIR:CW` / `DIR:CCW`
   - `E` (Paro de emergencia) / `RESET_E` (Restablecer)

---

## 🔋 Medición de corriente (prueba adicional)

El BTS7960 tiene pines de sensado **R_IS** y **L_IS** que entregan una corriente
proporcional a la del motor (`I_IS = I_motor / 8500`). Con una resistencia a GND
esa corriente se convierte en tensión, que la ESP32 lee con su ADC.

### Conexión

| Pin BTS7960 | Conexión |
| :--- | :--- |
| **R_IS** | Resistencia **680 Ω** a GND, y de ahí a **GPIO 34** (opcional: 1 kΩ en serie hacia el GPIO) |
| **L_IS** | Resistencia **680 Ω** a GND, y de ahí a **GPIO 35** (opcional: 1 kΩ en serie hacia el GPIO) |

```text
 R_IS ──┬──[1k opcional]── GPIO 34        L_IS ──┬──[1k opcional]── GPIO 35
        │                                        │
      [680Ω]                                   [680Ω]
        │                                        │
       GND                                      GND
```

> ⚠️ **No uses 1 kΩ o más a GND**: ante un fallo el BTS7960 saca ~4.5 mA por IS
> y con 1 kΩ serían 4.5 V, suficiente para dañar el pin de la ESP32. Con 680 Ω
> el máximo es ~3.06 V. Si usas otro valor, configúralo con `SET_RIS:<ohm>`.
> Si tu módulo ya trae resistencia en IS, mídela con el multímetro (sin
> alimentar) y tenla en cuenta: dos resistencias quedan en paralelo.

### Primer uso (por Monitor Serie o BLE)

1. Con el motor **detenido**, envía `CAL_I0` para calibrar el cero.
2. Arranca el motor (`START`, `SPD:200`). Cada segundo verás:
   `[CORRIENTE MEDIDA] Promedio: 1.85 A | Pico: 2.40 A | Consumido: 0.3 mAh | PWM: 200`
3. Compara con un multímetro/pinza en serie con la fuente. Si hay diferencia
   constante, ajusta el factor con `SET_KILIS:<valor>` (más alto = lee más corriente).
   Ej.: lee 1.85 A y el multímetro marca 2.00 A → `SET_KILIS:9189` (8500 × 2.00 / 1.85).

### Sin sensor: modo estimado

Si no conectas R_IS/L_IS, usa `CUR_MODE:EST`. La corriente se estima como
`I_nominal × PWM/255`. Mide una vez con el multímetro la corriente a PWM 255
con la carga habitual y guárdala con `SET_INOM:<mA>` (ej. `SET_INOM:2500`).
Es solo una aproximación: no detecta atascos ni cambios de carga.

### Comandos de corriente

| Comando | Función |
| :--- | :--- |
| `CUR` | Muestra la corriente actual |
| `CUR_RESET` | Reinicia el pico y los mAh acumulados |
| `CAL_I0` | Calibra el cero (con el motor detenido) |
| `CUR_MODE:SENSOR` / `CUR_MODE:EST` | Medición real por IS / estimación por PWM |
| `SET_RIS:<ohm>` | Resistencia de IS a GND (por defecto 680) |
| `SET_KILIS:<k>` | Factor de sensado del BTS7960 (por defecto 8500) |
| `SET_INOM:<mA>` | Corriente a PWM 255 para el modo estimado |
| `SET_ILIM:<mA>` | Paro de emergencia automático si se supera durante 300 ms (0 = desactivado) |
| `CUR_LOG:1` / `CUR_LOG:0` | Activa/desactiva el registro cada segundo en el Monitor Serie |

Todos los valores se guardan en la memoria flash. Por BLE la ESP32 envía cada
250 ms `CUR:<mA promedio>,<mA pico>,<mAh>,<S|E>` y, si actúa la protección,
`ALERT:OVERCURRENT`. La app actual ignora estos mensajes, así que sigue
funcionando igual.

---

## 📱 Paso 2: Compilar la App localmente (opcional)

Requisitos: Flutter 3.47 o superior, Android SDK y JDK 17.

```bash
flutter pub get
flutter run                     # con el teléfono conectado por USB (depuración USB activa)
flutter build apk --release     # genera build/app/outputs/flutter-apk/app-release.apk
```

## 🎛️ Uso de la App

- Pulsa **Buscar**: la app se conecta automáticamente a **`ESP32-MotorBTS`**.
- Acepta los permisos de Bluetooth (y ubicación en Android 11 o inferior).
- Controles: **slider de velocidad**, **sentido de giro**, **Marcha/Parar**,
  **PARO DE EMERGENCIA** y, desde el icono ⚙️, los parámetros del driver
  (frecuencia PWM, límites de velocidad, rampa e inversión de sentido) que se
  guardan en la memoria flash de la ESP32.
