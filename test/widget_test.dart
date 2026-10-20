import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:motor_controller/main.dart';

Future<dynamic> iniciarApp(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1080, 6000);
  tester.view.devicePixelRatio = 2.625;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(const MotorControllerApp());
  return tester.state(find.byType(MotorDashboard));
}

void main() {
  testWidgets('Muestra el panel de control desconectado', (WidgetTester tester) async {
    await tester.pumpWidget(const MotorControllerApp());

    expect(find.text('Control Motor BTS7960'), findsOneWidget);
    expect(find.text('Desconectado'), findsOneWidget);
    expect(find.text('PARO DE EMERGENCIA'), findsOneWidget);
    expect(find.text('Conéctate a la ESP32 para ver la gráfica'), findsOneWidget);
  });

  testWidgets('Los mensajes CUR muestran corriente, pico, lado y gráfica', (WidgetTester tester) async {
    final dynamic estado = await iniciarApp(tester);

    estado.procesarDatosEntrantes('CUR:640,720,R,1050');
    estado.procesarDatosEntrantes('CUR:655,720,R,1050');
    await tester.pump();

    expect(find.text('0.66 A'), findsOneWidget);
    expect(find.text('Pico: 0.72 A'), findsOneWidget);
    expect(find.text('Lado R (adelante)'), findsOneWidget);
    expect(find.text('Se detiene al superar 1.05 A'), findsOneWidget);
    expect(find.byType(LineChart), findsOneWidget);
    expect(find.textContaining('mAh'), findsNothing);
  });

  testWidgets('Al superar el límite se avisa y se puede soltar', (WidgetTester tester) async {
    final dynamic estado = await iniciarApp(tester);

    estado.procesarDatosEntrantes('ST:EMG,CW,150,0');
    estado.procesarDatosEntrantes('CUR:0,1120,-,1050');
    estado.procesarDatosEntrantes('ALERT:GRIP');
    await tester.pumpAndSettle();

    expect(find.text('Límite de corriente alcanzado'), findsOneWidget);
    expect(find.textContaining('1.05 A'), findsWidgets);
    expect(find.textContaining('pico 1.12 A'), findsOneWidget);
    expect(find.text('Soltar'), findsOneWidget);

    await tester.tap(find.text('Restablecer'));
    await tester.pumpAndSettle();
    expect(find.text('Límite de corriente alcanzado'), findsNothing);
    expect(find.text('Motor detenido'), findsOneWidget);
    expect(find.text('Restablecer Emergencia'), findsOneWidget);
  });
}
