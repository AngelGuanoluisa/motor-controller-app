import 'dart:math';

import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';

/// Ventana de tiempo que muestra la gráfica (segundos).
const double ventanaGraficaSeg = 60;

/// Tarjeta con la corriente actual, el consumo acumulado (mAh) frente al
/// límite y una gráfica de la corriente de los últimos [ventanaGraficaSeg] s.
class ConsumoCard extends StatelessWidget {
  const ConsumoCard({
    super.key,
    required this.corrienteMa,
    required this.picoMa,
    required this.consumoMah,
    required this.limiteMah,
    required this.esEstimada,
    required this.bloqueado,
    required this.historial,
    required this.onDesbloquear,
    required this.onReiniciarContador,
  });

  final double corrienteMa;
  final double picoMa;
  final double consumoMah;
  final int limiteMah;
  final bool esEstimada;
  final bool bloqueado;

  /// Puntos (segundos desde que se conectó, corriente en A).
  final List<FlSpot> historial;
  final VoidCallback? onDesbloquear;
  final VoidCallback? onReiniciarContador;

  @override
  Widget build(BuildContext context) {
    final progreso = limiteMah > 0 ? (consumoMah / limiteMah).clamp(0.0, 1.0) : 0.0;
    final colorProgreso = progreso >= 1
        ? Colors.redAccent
        : progreso >= 0.8
            ? Colors.orangeAccent
            : Colors.greenAccent;

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
                  label: Text(esEstimada ? 'Estimada' : 'Medida', style: const TextStyle(fontSize: 12)),
                  avatar: Icon(esEstimada ? Icons.calculate : Icons.sensors, size: 16),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Wrap(
              crossAxisAlignment: WrapCrossAlignment.end,
              spacing: 12,
              children: [
                Text('${(corrienteMa / 1000).toStringAsFixed(2)} A',
                    style: const TextStyle(fontSize: 32, fontWeight: FontWeight.bold, color: Colors.amberAccent)),
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: Text('Pico: ${(picoMa / 1000).toStringAsFixed(2)} A',
                      style: const TextStyle(color: Colors.white70)),
                ),
              ],
            ),
            const SizedBox(height: 16),

            // Consumo acumulado frente al límite
            Wrap(
              alignment: WrapAlignment.spaceBetween,
              spacing: 8,
              children: [
                const Text('Consumo acumulado'),
                Text(
                  limiteMah > 0
                      ? '${consumoMah.toStringAsFixed(1)} / $limiteMah mAh'
                      : '${consumoMah.toStringAsFixed(1)} mAh (sin límite)',
                  style: TextStyle(color: colorProgreso, fontWeight: FontWeight.bold),
                ),
              ],
            ),
            const SizedBox(height: 6),
            ClipRRect(
              borderRadius: BorderRadius.circular(6),
              child: LinearProgressIndicator(
                value: limiteMah > 0 ? progreso : 0,
                minHeight: 10,
                color: colorProgreso,
                backgroundColor: Colors.white12,
              ),
            ),

            if (bloqueado) ...[
              const SizedBox(height: 12),
              Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: Colors.red.withValues(alpha: 0.2),
                  borderRadius: BorderRadius.circular(10),
                  border: Border.all(color: Colors.redAccent),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    const Row(
                      children: [
                        Icon(Icons.lock, color: Colors.redAccent),
                        SizedBox(width: 8),
                        Expanded(
                          child: Text('Motor BLOQUEADO: se alcanzó el límite de consumo',
                              style: TextStyle(fontWeight: FontWeight.bold)),
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),
                    OutlinedButton.icon(
                      onPressed: onDesbloquear,
                      icon: const Icon(Icons.lock_open),
                      label: const Text('Reiniciar contador y desbloquear'),
                    ),
                  ],
                ),
              ),
            ] else
              Align(
                alignment: Alignment.centerRight,
                child: TextButton.icon(
                  onPressed: onReiniciarContador,
                  icon: const Icon(Icons.restart_alt, size: 18),
                  label: const Text('Reiniciar contador'),
                ),
              ),

            const SizedBox(height: 8),
            Text('Corriente (A) - últimos ${ventanaGraficaSeg.toInt()} s',
                style: const TextStyle(color: Colors.white70, fontSize: 12)),
            const SizedBox(height: 8),
            SizedBox(height: 180, child: _grafica()),
          ],
        ),
      ),
    );
  }

  Widget _grafica() {
    if (historial.isEmpty) {
      return const Center(child: Text('Sin datos todavía', style: TextStyle(color: Colors.white38)));
    }
    final maxX = max(historial.last.x, ventanaGraficaSeg);
    final minX = maxX - ventanaGraficaSeg;
    final maxValor = historial.map((p) => p.y).fold<double>(0, max);
    // Escala Y redondeada hacia arriba, mínimo 0.5 A
    final maxY = max(0.5, (maxValor * 1.2 * 2).ceil() / 2);

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
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipItems: (spots) => spots
                .map((s) => LineTooltipItem('${s.y.toStringAsFixed(2)} A',
                    const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)))
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
