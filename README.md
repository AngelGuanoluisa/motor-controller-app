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
