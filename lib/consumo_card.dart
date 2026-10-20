import 'dart:math';

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';

/// Ventana de tiempo que muestra la gráfica (segundos).
const double ventanaGraficaSeg = 60;

/// Tarjeta con la corriente medida por el BTS7960, el pico de la marcha
/// actual y una gráfica de los últimos [ventanaGraficaSeg] s con el límite.
class ConsumoCard extends StatelessWidget {
  const ConsumoCard({
    super.key,
    required this.corrienteMa,
    required this.picoMa,
    required this.limiteMa,
    required this.lado,
    required this.historial,
  });

  final double corrienteMa;
  final double picoMa;

  /// Límite de corriente que detiene el motor (0 = desactivado).
  final int limiteMa;

  /// Chip que está midiendo: 'R' (adelante), 'L' (atrás) o '-' (motor detenido).
  final String lado;

  /// Puntos (segundos desde que se conectó, corriente en mA).
  final List<FlSpot> historial;

  static String amperes(num mA) => '${(mA / 1000).toStringAsFixed(2)} A';

  @override
  Widget build(BuildContext context) {
    final cercaDelLimite = limiteMa > 0 && corrienteMa >= limiteMa * 0.8;

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Expanded(
                  child: Text('Consumo de Corriente', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                ),
                Chip(
                  visualDensity: VisualDensity.compact,
                  label: Text(_nombreLado, style: const TextStyle(fontSize: 12)),
                  avatar: const Icon(Icons.sensors, size: 16),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Wrap(
              crossAxisAlignment: WrapCrossAlignment.end,
              spacing: 12,
              children: [
                Text(
                  amperes(corrienteMa),
                  style: TextStyle(
                    fontSize: 32,
                    fontWeight: FontWeight.bold,
                    color: cercaDelLimite ? Colors.redAccent : Colors.amberAccent,
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: Text('Pico: ${amperes(picoMa)}', style: const TextStyle(color: Colors.white70)),
                ),
              ],
            ),
            const SizedBox(height: 4),
            Text(
              limiteMa > 0 ? 'Se detiene al superar ${amperes(limiteMa)}' : 'Límite de corriente desactivado',
              style: const TextStyle(color: Colors.white54, fontSize: 12),
            ),
            const SizedBox(height: 16),
            Text('Corriente (A) - últimos ${ventanaGraficaSeg.toInt()} s',
                style: const TextStyle(color: Colors.white70, fontSize: 12)),
            const SizedBox(height: 8),
            SizedBox(height: 180, child: _grafica()),
          ],
        ),
      ),
    );
  }

  String get _nombreLado => switch (lado) {
        'R' => 'Lado R (adelante)',
        'L' => 'Lado L (atrás)',
        _ => 'Motor detenido',
      };

  Widget _grafica() {
    if (historial.isEmpty) {
      return const Center(child: Text('Sin datos todavía', style: TextStyle(color: Colors.white38)));
    }
    final maxX = max(historial.last.x, ventanaGraficaSeg);
    final minX = maxX - ventanaGraficaSeg;
    final maxValor = historial.map((p) => p.y).fold<double>(0, max);
    // Escala Y en A redondeada a múltiplos de 0.25 A, mínimo 0.5 A
    final limiteA = limiteMa / 1000.0;
    final maxY = max(0.5, (max(maxValor, limiteA) * 1.15 / 0.25).ceil() * 0.25);

    return LineChart(
      LineChartData(
        minX: minX,
        maxX: maxX,
        minY: 0,
        maxY: maxY,
        clipData: const FlClipData.all(),
        gridData: FlGridData(
          drawVerticalLine: false,
          horizontalInterval: maxY / 4,
          getDrawingHorizontalLine: (_) => const FlLine(color: Colors.white12, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        titlesData: FlTitlesData(
          topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          bottomTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              interval: 15,
              reservedSize: 22,
              getTitlesWidget: (valor, meta) {
                // Segundos relativos al instante actual: -60, -45, ... 0
                final rel = (valor - maxX).round();
                return SideTitleWidget(
                  meta: meta,
                  child: Text('${rel}s', style: const TextStyle(fontSize: 10, color: Colors.white54)),
                );
              },
            ),
          ),
          leftTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              interval: maxY / 4,
              reservedSize: 36,
              getTitlesWidget: (valor, meta) => SideTitleWidget(
                meta: meta,
                child: Text(valor.toStringAsFixed(2), style: const TextStyle(fontSize: 10, color: Colors.white54)),
              ),
            ),
          ),
        ),
        extraLinesData: ExtraLinesData(
          horizontalLines: [
            if (limiteMa > 0)
              HorizontalLine(
                y: limiteA,
                color: Colors.redAccent,
                strokeWidth: 1.5,
                dashArray: [6, 4],
                label: HorizontalLineLabel(
                  show: true,
                  alignment: Alignment.topLeft,
                  style: const TextStyle(color: Colors.redAccent, fontSize: 10),
                  labelResolver: (_) => 'Límite ${amperes(limiteMa)}',
                ),
              ),
          ],
        ),
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipItems: (spots) => spots
                .map((s) => LineTooltipItem(
                    '${s.y.toStringAsFixed(2)} A', const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)))
                .toList(),
          ),
        ),
        lineBarsData: [
          LineChartBarData(
            spots: historial,
            isCurved: false,
            color: Colors.amberAccent,
            barWidth: 2,
            dotData: const FlDotData(show: false),
            belowBarData: BarAreaData(show: true, color: Colors.amberAccent.withValues(alpha: 0.15)),
          ),
        ],
      ),
      duration: Duration.zero,
    );
  }
}
