import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'consumo_card.dart';

void main() {
  runApp(const MotorControllerApp());
}

class MotorControllerApp extends StatelessWidget {
  const MotorControllerApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Control Motor BTS7960',
      theme: ThemeData.dark().copyWith(
        primaryColor: Colors.deepPurple,
        scaffoldBackgroundColor: const Color(0xFF121212),
        colorScheme: const ColorScheme.dark(
          primary: Colors.blueAccent,
          secondary: Colors.redAccent,
        ),
      ),
      home: const MotorDashboard(),
      debugShowCheckedModeBanner: false,
    );
  }
}

class MotorDashboard extends StatefulWidget {
  const MotorDashboard({super.key});

  @override
  State<MotorDashboard> createState() => _MotorDashboardState();
}

class _MotorDashboardState extends State<MotorDashboard> {
  static const String deviceName = "ESP32-MotorBTS";
  final String serviceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
  final String rxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
  final String txUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

  BluetoothDevice? targetDevice;
  BluetoothCharacteristic? rxCharacteristic;
  BluetoothCharacteristic? txCharacteristic;
  bool isConnected = false;
  bool isScanning = false;

  // Estados dinámicos de operación
  double speedValue = 150.0;
  double realSpeedValue = 0.0;
  bool isCw = true;
  bool isRunning = false;
  bool isEmergency = false;

  // Consumo de corriente (mensajes CUR: de la ESP32)
  double corrienteMa = 0;
  double picoMa = 0;
  int limiteCorrienteMa = 1050;
  String ladoActivo = '-';
  bool hayDatosCorriente = false;
  final List<FlSpot> historialCorriente = [];
  final Stopwatch relojGrafica = Stopwatch();

  // Parámetros de Configuración del Motor
  int currentFreq = 20000;
  int currentMinSpeed = 0;
  int currentMaxSpeed = 255;
  int currentRampMs = 500;
  bool isInverted = false;

  StreamSubscription? connectionSubscription;
  StreamSubscription? txSubscription;
  StreamSubscription? scanSubscription;
  bool isConnecting = false;

  @override
  void initState() {
    super.initState();
    solicitarPermisos();
  }

  Future<void> solicitarPermisos() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
  }

  void mostrarMensaje(String texto) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(texto)));
  }

  Future<void> startScan() async {
    if (isScanning || isConnecting) return;

    if (await FlutterBluePlus.isSupported == false) {
      mostrarMensaje('Este teléfono no soporta Bluetooth Low Energy');
      return;
    }
    if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
      try {
        await FlutterBluePlus.turnOn();
      } catch (_) {
        mostrarMensaje('Activa el Bluetooth para buscar la ESP32');
        return;
      }
    }

    setState(() => isScanning = true);

    await scanSubscription?.cancel();
    scanSubscription = FlutterBluePlus.onScanResults.listen((results) {
      for (ScanResult r in results) {
        if (r.device.platformName == deviceName || r.advertisementData.advName == deviceName) {
          scanSubscription?.cancel();
          scanSubscription = null;
          FlutterBluePlus.stopScan();
          connectToDevice(r.device);
          break;
        }
      }
    });

    try {
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 4));
      await FlutterBluePlus.isScanning.where((scanning) => scanning == false).first;
    } catch (e) {
      mostrarMensaje('Error al buscar: $e');
    }

    await scanSubscription?.cancel();
    scanSubscription = null;
    if (!mounted) return;
    setState(() => isScanning = false);
    if (!isConnected && !isConnecting) {
      mostrarMensaje('No se encontró $deviceName. ¿Está encendida la ESP32?');
    }
  }

  Future<void> connectToDevice(BluetoothDevice device) async {
    if (isConnecting) return;
    isConnecting = true;
    targetDevice = device;

    await connectionSubscription?.cancel();
    connectionSubscription = device.connectionState.listen((state) {
      final conectado = state == BluetoothConnectionState.connected;
      if (!conectado) {
        txSubscription?.cancel();
        txSubscription = null;
        rxCharacteristic = null;
        txCharacteristic = null;
      }
      if (mounted) setState(() => isConnected = conectado);
    });

    try {
      await device.connect(autoConnect: false, timeout: const Duration(seconds: 10));
      await descubrirServicios(device);
    } catch (e) {
      mostrarMensaje('No se pudo conectar: $e');
    } finally {
      isConnecting = false;
    }
  }

  Future<void> descubrirServicios(BluetoothDevice device) async {
    final servicioUart = Guid(serviceUuid);
    final caracRx = Guid(rxUuid);
    final caracTx = Guid(txUuid);

    // Android guarda en caché los servicios BLE de cada dispositivo. Si la ESP32
    // tuvo antes otro firmware, el teléfono puede seguir viendo los servicios
    // viejos; por eso se limpia la caché y se reintenta.
    List<BluetoothService> services = [];
    for (int intento = 1; intento <= 3; intento++) {
      if (Platform.isAndroid) {
        try {
          await device.clearGattCache();
        } catch (_) {}
      }
      services = await device.discoverServices();
      if (services.any((s) => s.uuid == servicioUart)) break;
      await Future.delayed(const Duration(milliseconds: 600));
    }

    for (var s in services.where((s) => s.uuid == servicioUart)) {
      for (var c in s.characteristics) {
        if (c.uuid == caracRx) {
          rxCharacteristic = c;
        }
        if (c.uuid == caracTx) {
          txCharacteristic = c;
          await txSubscription?.cancel();
          txSubscription = c.onValueReceived.listen((data) {
            procesarBytesEntrantes(data);
          });
          device.cancelWhenDisconnected(txSubscription!);
          await c.setNotifyValue(true);
        }
      }
    }
    if (rxCharacteristic == null) {
      mostrarDiagnosticoServicios(services);
      return;
    }
    // Solicitar configuración actual
    Future.delayed(const Duration(milliseconds: 300), () => enviarComando("GET_CFG"));
  }

  void mostrarDiagnosticoServicios(List<BluetoothService> services) {
    if (!mounted) return;
    final lista = services.isEmpty
        ? '(ningún servicio)'
        : services
            .map((s) => '• ${s.uuid.str128}\n'
                '${s.characteristics.map((c) => '    - ${c.uuid.str128}').join('\n')}')
            .join('\n');
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Servicio BLE no encontrado'),
        content: SingleChildScrollView(
          child: Text(
            'La app busca el servicio:\n$serviceUuid\n\n'
            'El dispositivo "${targetDevice?.platformName}" ofrece:\n$lista\n\n'
            'Posibles causas:\n'
            '1. La ESP32 no tiene cargado el firmware de este proyecto.\n'
            '2. El teléfono recuerda los servicios de un firmware anterior: '
            'desactiva y activa el Bluetooth del teléfono, o reinicia el teléfono.',
          ),
        ),
        actions: [
          TextButton(
            onPressed: () {
              Navigator.pop(context);
              targetDevice?.disconnect();
            },
            child: const Text('Desconectar'),
          ),
        ],
      ),
    );
  }

  // La ESP32 envía cada mensaje en una notificación (sin '\n'). Al conectar,
  // flutter_blue_plus pide MTU 512 en Android, así que los mensajes llegan completos.
  void procesarBytesEntrantes(List<int> data) {
    final texto = utf8.decode(data, allowMalformed: true);
    for (final linea in texto.split('\n')) {
      final l = linea.trim();
      if (l.isNotEmpty) procesarDatosEntrantes(l);
    }
  }

  void procesarDatosEntrantes(String raw) {
    // 1. Telemetría de estado: ST:<RUN/STOP/EMG>,<CW/CCW>,<spdObj>,<spdAct>
    if (raw.startsWith("ST:")) {
      var partes = raw.substring(3).split(",");
      if (partes.length == 4) {
        setState(() {
          isEmergency = partes[0] == "EMG";
          isRunning = partes[0] == "RUN";
          isCw = partes[1] == "CW";
          speedValue = double.tryParse(partes[2]) ?? speedValue;
          realSpeedValue = double.tryParse(partes[3]) ?? realSpeedValue;
        });
      }
    }
    // 2. Parámetros de configuración: CFG:<freq>,<spdMin>,<spdMax>,<ramp>,<invDir>
    else if (raw.startsWith("CFG:")) {
      var params = raw.substring(4).split(",");
      if (params.length == 5) {
        setState(() {
          currentFreq = int.tryParse(params[0]) ?? currentFreq;
          currentMinSpeed = int.tryParse(params[1]) ?? currentMinSpeed;
          currentMaxSpeed = int.tryParse(params[2]) ?? currentMaxSpeed;
          currentRampMs = int.tryParse(params[3]) ?? currentRampMs;
          isInverted = params[4] == "1";
        });
      }
    }
    // 3. Corriente: CUR:<mA actual>,<mA pico>,<lado R|L|->,<límite mA>
    else if (raw.startsWith("CUR:")) {
      var c = raw.substring(4).split(",");
      if (c.length >= 4) {
        if (!relojGrafica.isRunning) relojGrafica.start();
        final t = relojGrafica.elapsedMilliseconds / 1000.0;
        setState(() {
          hayDatosCorriente = true;
          corrienteMa = double.tryParse(c[0]) ?? corrienteMa;
          picoMa = double.tryParse(c[1]) ?? picoMa;
          ladoActivo = c[2];
          limiteCorrienteMa = int.tryParse(c[3]) ?? limiteCorrienteMa;
          historialCorriente.add(FlSpot(t, corrienteMa / 1000.0));
          // Conservar solo la ventana visible de la gráfica
          while (historialCorriente.isNotEmpty && historialCorriente.first.x < t - ventanaGraficaSeg - 1) {
            historialCorriente.removeAt(0);
          }
        });
      }
    }
    // 4. Alertas de protección
    else if (raw.startsWith("ALERT:")) {
      if (raw.substring(6) == "GRIP") mostrarAlertaLimite();
    }
  }

  void mostrarAlertaLimite() {
    if (!mounted) return;
    final sentidoSoltar = isCw ? 'CCW' : 'CW';
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        icon: const Icon(Icons.pan_tool, color: Colors.amberAccent, size: 40),
        title: const Text('Límite de corriente alcanzado'),
        content: Text(
          'La corriente superó ${ConsumoCard.amperes(limiteCorrienteMa)} '
          '(pico ${ConsumoCard.amperes(picoMa)}): el motor se detuvo con paro de emergencia.',
        ),
        actions: [
          TextButton(
            onPressed: () {
              Navigator.pop(context);
              enviarComando('RESET_E');
            },
            child: const Text('Restablecer'),
          ),
          FilledButton.icon(
            icon: const Icon(Icons.open_with),
            onPressed: () async {
              Navigator.pop(context);
              await enviarComando('RESET_E');
              await enviarComando('DIR:$sentidoSoltar');
              await enviarComando('START');
            },
            label: const Text('Soltar'),
          ),
        ],
      ),
    );
  }

  Future<void> enviarComando(String comando) async {
    final rx = rxCharacteristic;
    if (rx == null || !isConnected) return;
    try {
      await rx.write(utf8.encode("$comando\n"), withoutResponse: false);
    } catch (e) {
      mostrarMensaje('Error al enviar "$comando": $e');
    }
  }

  void abrirDialogoConfiguracion() {
    final freqCtrl = TextEditingController(text: currentFreq.toString());
    final minSpdCtrl = TextEditingController(text: currentMinSpeed.toString());
    final maxSpdCtrl = TextEditingController(text: currentMaxSpeed.toString());
    final rampCtrl = TextEditingController(text: currentRampMs.toString());
    final limiteCtrl = TextEditingController(text: (limiteCorrienteMa / 1000).toStringAsFixed(2));
    bool tempInvert = isInverted;

    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      backgroundColor: const Color(0xFF1E1E1E),
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setModalState) {
            return Padding(
              padding: EdgeInsets.only(
                top: 20,
                left: 20,
                right: 20,
                bottom: MediaQuery.of(context).viewInsets.bottom + 20,
              ),
              child: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      mainAxisAlignment: MainAxisAlignment.spaceBetween,
                      children: [
                        const Text(
                          'Ajustes del Driver y Motor',
                          style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold, color: Colors.blueAccent),
                        ),
                        IconButton(
                          icon: const Icon(Icons.close),
                          onPressed: () => Navigator.pop(context),
                        )
                      ],
                    ),
                    const Divider(),
                    const SizedBox(height: 10),

                    // Frecuencia PWM (Hz)
                    TextField(
                      controller: freqCtrl,
                      keyboardType: TextInputType.number,
                      decoration: const InputDecoration(
                        labelText: 'Frecuencia PWM (Hz)',
                        helperText: 'Típico: 20000 Hz (silencioso), 1000 Hz, etc.',
                        border: OutlineInputBorder(),
                        prefixIcon: Icon(Icons.waves),
                      ),
                    ),
                    const SizedBox(height: 16),

                    // Límites de velocidad Min y Max
                    Row(
                      children: [
                        Expanded(
                          child: TextField(
                            controller: minSpdCtrl,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(
                              labelText: 'Vel. Mínima (0-255)',
                              border: OutlineInputBorder(),
                            ),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: TextField(
                            controller: maxSpdCtrl,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(
                              labelText: 'Vel. Máxima (0-255)',
                              border: OutlineInputBorder(),
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 16),

                    // Rampa de Aceleración en ms
                    TextField(
                      controller: rampCtrl,
                      keyboardType: TextInputType.number,
                      decoration: const InputDecoration(
                        labelText: 'Tiempo de rampa (ms)',
                        helperText: 'Suavidad de arranque/frenado (0 = instantáneo)',
                        border: OutlineInputBorder(),
                        prefixIcon: Icon(Icons.timeline),
                      ),
                    ),
                    const SizedBox(height: 12),

                    // Invertir sentido lógico
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: const Text('Invertir sentido lógico (CW <-> CCW)'),
                      subtitle: const Text('Corrige sentido sin cambiar cables del motor'),
                      value: tempInvert,
                      onChanged: (val) {
                        setModalState(() => tempInvert = val);
                      },
                    ),

                    const SizedBox(height: 12),

                    // Límite de corriente
                    TextField(
                      controller: limiteCtrl,
                      keyboardType: const TextInputType.numberWithOptions(decimal: true),
                      decoration: const InputDecoration(
                        labelText: 'Límite de corriente (A)',
                        helperText: 'Al superarlo el motor se detiene (0 = desactivado)',
                        border: OutlineInputBorder(),
                        prefixIcon: Icon(Icons.pan_tool),
                      ),
                    ),

                    const SizedBox(height: 20),

                    // Botón Guardar
                    SizedBox(
                      width: double.infinity,
                      height: 50,
                      child: ElevatedButton.icon(
                        style: ElevatedButton.styleFrom(
                          backgroundColor: Colors.blueAccent,
                          foregroundColor: Colors.white,
                        ),
                        icon: const Icon(Icons.save),
                        label: const Text('GUARDAR EN ESP32 (FLASH NVS)', style: TextStyle(fontWeight: FontWeight.bold)),
                        onPressed: () {
                          int? f = int.tryParse(freqCtrl.text);
                          int? sMin = int.tryParse(minSpdCtrl.text);
                          int? sMax = int.tryParse(maxSpdCtrl.text);
                          int? r = int.tryParse(rampCtrl.text);

                          if (f != null) enviarComando('SET_FREQ:$f');
                          if (sMin != null && sMax != null) enviarComando('SET_LIMITS:$sMin,$sMax');
                          if (r != null) enviarComando('SET_RAMP:$r');
                          enviarComando('SET_INVERT:${tempInvert ? 1 : 0}');
                          double? limiteA = double.tryParse(limiteCtrl.text.replaceAll(',', '.'));
                          if (limiteA != null && limiteA >= 0) {
                            enviarComando('SET_ILIM:${(limiteA * 1000).round()}');
                          }

                          Navigator.pop(context);
                          ScaffoldMessenger.of(context).showSnackBar(
                            const SnackBar(content: Text('Parámetros enviados y guardados en memoria del ESP32')),
                          );
                        },
                      ),
                    )
                  ],
                ),
              ),
            );
          },
        );
      },
    );
  }

  @override
  void dispose() {
    scanSubscription?.cancel();
    connectionSubscription?.cancel();
    txSubscription?.cancel();
    targetDevice?.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Control Motor BTS7960'),
        actions: [
          // Botón de Configuración de Parámetros
          IconButton(
            icon: const Icon(Icons.tune),
            tooltip: 'Configurar parámetros',
            onPressed: isConnected ? abrirDialogoConfiguracion : null,
          ),
          IconButton(
            icon: Icon(isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
                color: isConnected ? Colors.greenAccent : Colors.grey),
            onPressed: isConnected ? () => targetDevice?.disconnect() : startScan,
          )
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          children: [
            // Estado de Conexión y Frecuencia Activa
            Card(
              color: isConnected ? Colors.green.withValues(alpha: 0.15) : Colors.red.withValues(alpha: 0.15),
              child: ListTile(
                leading: Icon(isConnected ? Icons.check_circle : Icons.warning,
                    color: isConnected ? Colors.greenAccent : Colors.orangeAccent),
                title: Text(isConnected ? 'Conectado a ESP32-MotorBTS' : 'Desconectado'),
                subtitle: isConnected ? Text('Frecuencia: $currentFreq Hz | Rampa: ${currentRampMs}ms') : null,
                trailing: isScanning
                    ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
                    : ElevatedButton(
                        onPressed: isConnected ? null : startScan,
                        child: const Text('Buscar'),
                      ),
              ),
            ),
            const SizedBox(height: 16),

            // BOTÓN PARO DE EMERGENCIA
            SizedBox(
              width: double.infinity,
              height: 70,
              child: ElevatedButton.icon(
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.red[800],
                  foregroundColor: Colors.white,
                  elevation: 6,
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
                ),
                icon: const Icon(Icons.emergency, size: 36),
                label: const Text('PARO DE EMERGENCIA',
                    style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold, letterSpacing: 1.2)),
                onPressed: () => enviarComando('E'),
              ),
            ),
            if (isEmergency)
              Padding(
                padding: const EdgeInsets.only(top: 8.0),
                child: TextButton.icon(
                  onPressed: () => enviarComando('RESET_E'),
                  icon: const Icon(Icons.refresh, color: Colors.amber),
                  label: const Text('Restablecer Emergencia', style: TextStyle(color: Colors.amber)),
                ),
              ),

            const SizedBox(height: 24),

            // MARCHA Y DETENCIÓN
            Row(
              children: [
                Expanded(
                  child: SizedBox(
                    height: 55,
                    child: ElevatedButton.icon(
                      style: ElevatedButton.styleFrom(
                        backgroundColor: isRunning ? Colors.green[700] : Colors.grey[800],
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                      ),
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('MARCHA'),
                      onPressed: isConnected && !isEmergency ? () => enviarComando('START') : null,
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: SizedBox(
                    height: 55,
                    child: ElevatedButton.icon(
                      style: ElevatedButton.styleFrom(
                        backgroundColor: !isRunning && !isEmergency ? Colors.blueGrey : Colors.grey[800],
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                      ),
                      icon: const Icon(Icons.stop),
                      label: const Text('PARAR'),
                      onPressed: isConnected && !isEmergency ? () => enviarComando('STOP') : null,
                    ),
                  ),
                ),
              ],
            ),

            const SizedBox(height: 24),

            // SENTIDO DE GIRO
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text('Sentido de Giro', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(height: 12),
                    Row(
                      children: [
                        Expanded(
                          child: ChoiceChip(
                            label: const Center(child: Text('Horario (CW)')),
                            selected: isCw,
                            onSelected: isConnected && !isEmergency
                                ? (val) {
                                    if (val) enviarComando('DIR:CW');
                                  }
                                : null,
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: ChoiceChip(
                            label: const Center(child: Text('Antihorario (CCW)')),
                            selected: !isCw,
                            onSelected: isConnected && !isEmergency
                                ? (val) {
                                    if (val) enviarComando('DIR:CCW');
                                  }
                                : null,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),

            const SizedBox(height: 16),

            // CONTROL DE VELOCIDAD
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  children: [
                    Wrap(
                      alignment: WrapAlignment.spaceBetween,
                      spacing: 8,
                      runSpacing: 4,
                      children: [
                        const Text('Velocidad PWM', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                        Text('${speedValue.round()} (${((speedValue / 255.0) * 100).toStringAsFixed(0)}%) [Real: ${realSpeedValue.round()}]',
                            style: const TextStyle(fontSize: 15, color: Colors.blueAccent)),
                      ],
                    ),
                    Slider(
                      value: speedValue.clamp(currentMinSpeed.toDouble(), currentMaxSpeed.toDouble()),
                      min: currentMinSpeed.toDouble(),
                      max: currentMaxSpeed.toDouble(),
                      divisions: (currentMaxSpeed - currentMinSpeed) > 0 ? (currentMaxSpeed - currentMinSpeed) : 1,
                      label: speedValue.round().toString(),
                      onChanged: isConnected && !isEmergency
                          ? (value) {
                              setState(() => speedValue = value);
                            }
                          : null,
                      onChangeEnd: isConnected && !isEmergency
                          ? (value) {
                              enviarComando('SPD:${value.round()}');
                            }
                          : null,
                    ),
                  ],
                ),
              ),
            ),

            const SizedBox(height: 16),

            // CONSUMO DE CORRIENTE
            if (hayDatosCorriente)
              ConsumoCard(
                corrienteMa: corrienteMa,
                picoMa: picoMa,
                limiteMa: limiteCorrienteMa,
                lado: ladoActivo,
                historial: List.unmodifiable(historialCorriente),
              )
            else
              const Card(
                child: ListTile(
                  leading: Icon(Icons.show_chart),
                  title: Text('Consumo de Corriente'),
                  subtitle: Text('Conéctate a la ESP32 para ver la gráfica'),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
