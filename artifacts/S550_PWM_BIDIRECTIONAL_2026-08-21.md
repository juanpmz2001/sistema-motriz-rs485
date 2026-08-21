# Tabla bidireccional PWM–RPM S550 — 2026-08-21

Velocidad del eje de salida medida con AS5600. RPM del S550 inferidas usando la reducción confirmada 108/15 = 7,2:1. El signo identifica el sentido del encoder.

| Separación desde 1500 us | PWM sentido A | RPM salida A | RPM S550 A | PWM sentido B | RPM salida B | RPM S550 B |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1500 | 0,00 | 0,00 | 1500 | 0,00 | 0,00 |
| 50 | 1450 | +0,01 | +0,08 | 1550 | −0,01 | −0,07 |
| 100 | 1400 | +0,50 | +3,57 | 1600 | −0,00 | −0,01 |
| 150 | 1350 | +1,06 | +7,65 | 1650 | −0,74 | −5,30 |
| 180 | — | — | — | 1680 | −1,07 | −7,73 |
| 200 | 1300 | +1,58 | +11,38 | 1700 | −1,20 | −8,63 |
| 250 | 1250 | +2,08 | +14,97 | 1750 | −1,70 | −12,24 |
| 300 | 1200 | +2,70 | +19,43 | 1800 | −2,34 | −16,83 |
| 350 | 1150 | +3,18 | +22,90 | 1850 | −3,01 | −21,65 |
| 400 | 1100 | +3,76 | +27,04 | 1900 | −3,40 | −24,48 |
| 460 | 1040 | +4,67 | +33,63 | 1960 | −4,18 | −30,07 |
| 500 | 1000 | +5,20 | +37,41 | 2000 | −4,86 | −34,97 |
| 600 | 900 | +6,67 | +47,99 | 2100 | −4,32 | −31,08 |
| 700 | 800 | +7,66 | +55,18 | 2200 | −5,69 | −40,94 |
| 800 | 700 | +8,48 | +61,04 | 2300 | −5,72 | −41,17 |
| 900 | 600 | +8,89 | +64,01 | 2400 | −8,32 | −59,90 |
| 950 | 550 | +8,98 | +64,68 | 2450 | −8,91 | −64,17 |
| 1000 | 500 | +8,93 | +64,27 | 2500 | −8,82 | −63,52 |

## Extremos medidos

- Sentido A, ángulo creciente: máximo **+64,68 rpm S550** a **550 us**; salida **+8,98 rpm**.
- Sentido B, ángulo decreciente: máximo **−64,17 rpm S550** a **2450 us**; salida **−8,91 rpm**.
- Límites PWM del manual: 500 us = +64,27 rpm medidos; 2500 us = −63,52 rpm medidos.
- Zona neutra observada: 1450, 1500, 1550 y 1600 us quedaron prácticamente en cero. El movimiento claro comienza cerca de 1400 us en A y 1650 us en B.

## Método y cautelas

- Cada nivel: rampa de 0,6 s, asentamiento de 1 s y meseta de 3 s.
- Pendiente OLS del ángulo corregido/desenvuelto entre 0,25 y 2,80 s de cada meseta.
- La rama A se midió en dos registros consecutivos; la rama B en un barrido hasta 2400 us y una repetición protegida de 2450/2500 us.
- Los puntos son mediciones del montaje completo y conservan sus variaciones reales de carga/posición; no se forzó una curva monótona.
- La primera pasada se detuvo de forma segura ante un salto imposible del encoder en 2450 us; ese tramo parcial se excluyó y fue sustituido por la repetición completa.
