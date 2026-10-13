import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:motor_controller/main.dart';

void main() {
  testWidgets('Muestra el panel de control desconectado', (WidgetTester tester) async {
    await tester.pumpWidget(const MotorControllerApp());

    expect(find.text('Control Motor BTS7960'), findsOneWidget);
    expect(find.text('Desconectado'), findsOneWidget);
    expect(find.text('PARO DE EMERGENCIA'), findsOneWidget);
    expect(find.text('Conéctate a la ESP32 para ver la gráfica'), findsOneWidget);
  });

  testWidgets('Los mensajes CUR muestran corriente, consumo y gráfica', (WidgetTester tester) async {
    tester.view.physicalSize = const Size(1080, 6000);
    tester.view.devicePixelRatio = 2.625;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(const MotorControllerApp());
    final dynamic estado = tester.state(find.byType(MotorDashboard));

    estado.procesarDatosEntrantes('CUR:1500,2100,123.4,S,750,0');
    estado.procesarDatosEntrantes('CUR:1600,2100,123.5,S,750,0');
    await tester.pump();

    expect(find.text('1.60 A'), findsOneWidget);
    expect(find.text('Pico: 2.10 A'), findsOneWidget);
    expect(find.text('123.5 / 750 mAh'), findsOneWidget);
    expect(find.byType(LineChart), findsOneWidget);
    expect(find.textContaining('BLOQUEADO'), findsNothing);
  });

  testWidgets('Al llegar al límite de mAh se muestra el bloqueo', (WidgetTester tester) async {
    tester.view.physicalSize = const Size(1080, 6000);
    tester.view.devicePixelRatio = 2.625;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(const MotorControllerApp());
    final dynamic estado = tester.state(find.byType(MotorDashboard));

    estado.procesarDatosEntrantes('ST:EMG,CW,150,0');
    estado.procesarDatosEntrantes('CUR:0,2100,750.0,S,750,1');
    estado.procesarDatosEntrantes('ALERT:MAH_LIMIT');
    await tester.pumpAndSettle();

    expect(find.text('Límite de consumo alcanzado'), findsOneWidget);
    await tester.tap(find.text('Entendido'));
    await tester.pumpAndSettle();

    expect(find.text('Motor BLOQUEADO: se alcanzó el límite de consumo'), findsOneWidget);
    expect(find.text('Reiniciar contador y desbloquear'), findsOneWidget);
    // RESET_E no sirve mientras está bloqueado: el botón se oculta
    expect(find.text('Restablecer Emergencia'), findsNothing);
  });
}
