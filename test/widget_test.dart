import 'package:flutter_test/flutter_test.dart';
import 'package:motor_controller/main.dart';

void main() {
  testWidgets('Muestra el panel de control desconectado', (WidgetTester tester) async {
    await tester.pumpWidget(const MotorControllerApp());

    expect(find.text('Control Motor BTS7960'), findsOneWidget);
    expect(find.text('Desconectado'), findsOneWidget);
    expect(find.text('PARO DE EMERGENCIA'), findsOneWidget);
  });
}
