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
├── lib/consumo_card.dart             # Tarjeta y gráfica de corriente
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
| **VCC** | 3V3 | Alimentación de la lógica del módulo |
| **GND** | GND | Tierra compartida (**¡común con la fuente del motor!**) |
| **RPWM** | GPIO 25 | PWM giro adelante (CW) |
| **LPWM** | GPIO 26 | PWM giro atrás (CCW) |
| **R_EN** | GPIO 27 | Habilitar lado R |
| **L_EN** | GPIO 14 | Habilitar lado L |
| **R_IS** | GPIO 34 | Sensado de corriente lado R (ADC1, solo entrada) |
| **L_IS** | GPIO 35 | Sensado de corriente lado L (ADC1, solo entrada) |
| **B+ / B-** | Fuente Externa | Alimentación del motor |
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

## 🔋 Medición de corriente y límite

La corriente se mide con las salidas **R_IS / L_IS** del BTS7960 (el IBT-2 trae
1 kΩ en cada pin IS):

```text
I_motor = (V_IS / R_IS) × K_ILIS
```

El módulo tiene **dos chips** (uno por lado) y cada uno está calibrado por
separado. Solo mide el chip del lado que está conduciendo:

| Sentido | Pin | Constante calibrada |
| :--- | :--- | :--- |
| Adelante (RPWM) | R_IS → GPIO 34 | `K_ILIS_R = 566.0` |
| Atrás (LPWM) | L_IS → GPIO 35 | `K_ILIS_L = 793.5` |

Cada lectura promedia 32 muestras del ADC. El offset de cada pin se calibra
automáticamente al encender, con el motor detenido (`CAL_I0` lo repite).

### Límite de corriente (1.05 A)

Si la corriente supera **1.05 A** durante **150 ms** seguidos, se activa el
**paro de emergencia** y el motor se detiene. Se ignoran los primeros
**500 ms** tras arrancar, cambiar de sentido o acelerar, porque el arranque
siempre pide un pico de corriente.

- El límite se cambia con `SET_ILIM:<mA>` (ej. `SET_ILIM:1050`) o desde los
  ajustes ⚙️ de la app (en amperios). `0` lo desactiva.
- Para continuar: `RESET_E`, cambiar de sentido y `START`. En la app, el aviso
  tiene un botón **Soltar** que lo hace.

### Comandos de corriente

| Comando | Función |
| :--- | :--- |
| `CUR` | Muestra la corriente actual (mV y A de cada lado) |
| `SET_ILIM:<mA>` | Límite de corriente (por defecto 1050, 0 = desactivado) |
| `SET_TLIM:<ms>` | Tiempo por encima del límite para detener (por defecto 150) |
| `SET_TBLANK:<ms>` | Tiempo ignorado tras arrancar / cambiar de sentido (por defecto 500) |
| `CAL_I0` | Recalibra el offset (con el motor detenido) |
| `CUR_LOG:1` / `CUR_LOG:0` | Activa/desactiva el registro cada segundo en el Monitor Serie |

Por BLE la ESP32 envía cada 250 ms `CUR:<mA actual>,<mA pico>,<lado R|L|->,<límite mA>`
y, cuando actúa el límite, `ALERT:GRIP`.

### 📈 En la app

La tarjeta **Consumo de Corriente** muestra la corriente actual, el pico de la
marcha, qué lado está midiendo y una **gráfica de los últimos 60 s** con una
línea roja en el límite.

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
