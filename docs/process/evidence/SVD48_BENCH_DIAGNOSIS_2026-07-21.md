# Diagnostico de banco SVD48 + KK16

Date: 2026-07-21

Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`

## Alcance y condiciones

- ESP32-S3 alimentado por el robot y operado por maintenance LAN.
- SVD48 dual en ID `2`; M1 y M2 conectados a motores KK16.
- Ruedas elevadas y autorizacion explicita para ejecutar movimiento.
- Firmware ESP build 19, con limite absoluto de `15 RPM` tanto en el control
  como en el gateway. Un comando de `16 RPM` fue rechazado antes de mover.
- La velocidad de `0x5410/0x5411` se registra como palabra cruda y como RPM
  aplicando la escala oficial `0.1 RPM`.

## Configuracion encontrada antes de calibrar

La captura completa previa esta en:

- `svd48_id2_inventory_2026-07-21_pre_calibration.json`
- `svd48_id2_inventory_2026-07-21_pre_calibration.md`

Valores relevantes:

| Parametro | M1 | M2 | Observacion |
| --- | ---: | ---: | --- |
| Pares de polos | 10 | 10 | Coincide con el perfil UUMOTOR usado |
| Sensor | Hall | Hall | Correcto para KK16 |
| KV | 16.6 RPM/V | 16.6 RPM/V | No fue identificado por la rutina L/R |
| Maximo RPM inicial | 10 | 10 | Luego se cambio y guardo en 15/15 |
| Aceleracion/deceleracion | 3/3 RPM/s | 3/3 RPM/s | Persistido previamente |
| Corriente maxima inicial | 30 A | 30 A | Reducida temporalmente a 5/5 A, sin guardar |
| Gear driving/driven | 1/5 | 1/5 | Registros reales `0x5030/31/34/35` |
| PID velocidad | 0.3 / 0.1 / 2.0 | 0.3 / 0.1 / 2.0 | Kp/Ki/Kd |
| PID aceleracion | 20 / 1 / 10 | 0.0015 / 0.0001 / 0 | M1 estaba fuera del perfil oficial |

El inventario historico buscaba gear en `0x2202/0x2203`; esos registros son
invalidos en software SVD48 `0x0131`. El XML oficial de SV-Config define los
campos por canal en `0x5030/0x5034` y `0x5031/0x5035`.

## Calibracion Hall repetida

Se ejecuto una calibracion automatica independiente por canal y se leyo
directamente el SVD48 durante todo el movimiento:

- M1: `svd48_hall_m1_direct_2026-07-21.csv`
- M2: `svd48_hall_m2_direct_2026-07-21.csv`

Ambos canales recorrieron los seis sectores Hall y poblaron tablas con separacion
aproximada de 60 grados. Sin embargo, ambos terminaron en estado de calibracion
`2`, el mismo resultado no satisfactorio observado en intentos anteriores. No se
debe afirmar que la calibracion Hall quedo aprobada.

`svd48_hall_m1_2026-07-21.csv` queda conservado como evidencia de un primer
intento, pero sus columnas logicas M1/M2 estan desplazadas porque el firmware
mapea los motores del drive ausente ID 1 antes de los del ID 2. Para analizar el
controlador se deben usar las capturas `*_direct_*`.

## Ensayos de velocidad

Se redujo temporalmente la corriente maxima a `5 A` por canal y se compararon
varias configuraciones. Estas escrituras no se guardaron en FLASH.

| Archivo | Ensayo | Resultado principal |
| --- | --- | --- |
| `svd48_speed_m1_smoke_2026-07-21.csv` | M1, 1 RPM, limite 30 A | Pico de 19.6 A aun sin carga; configuracion inaceptable para seguir probando |
| `svd48_speed_m2_smoke_5a_2026-07-21.csv` | M2, 1 RPM, limite 5 A | Oscilacion alrededor del objetivo; pico aproximado 5.5 A |
| `svd48_speed_m1_smoke_5a_bad_pid_2026-07-21.csv` | M1 con PID aceleracion heredado | Oscilacion y corriente alta para movimiento sin carga |
| `svd48_speed_m1_smoke_5a_acc_pid_fixed_2026-07-21.csv` | M1 con PID aceleracion del perfil oficial | Menor agresividad, pero no elimina el traqueteo |
| `svd48_speed_m1_smoke_5a_profile_pid_2026-07-21.csv` | M1, PID velocidad perfil oficial | Mejora parcial; persiste velocidad irregular |
| `svd48_speed_m2_smoke_5a_profile_pid_2026-07-21.csv` | M2, PID velocidad perfil oficial | Persiste velocidad irregular |
| `svd48_speed_m1_profile_pid_sweep_2026-07-21.csv` | M1, objetivos 3/5/10/15 RPM | El given RPM interno avanzo lentamente hasta 5 por el ramp; sin fault, velocidad cruda irregular |

Perfil PID temporal aplicado para comparar:

- Speed PID M1/M2: `Kp=0.25`, `Ki=0.1`, `Kd=0.5`.
- Speed acceleration PID M1/M2: `Kp=0.0015`, `Ki=0.0001`, `Kd=0`.
- Speed dead zone M1/M2: `1`.
- Current command filter M1/M2: `3000`.

La mejora de PID no corrigio la causa base. Hall, escala/unidad del comando y
semantica de gear deben verificarse con medicion fisica de vueltas.

## Prueba de relacion de engranajes

Se probaron relaciones temporales sin guardarlas:

1. `1/1`: `svd48_speed_m1_gear_1_to_1_1rpm_2026-07-21.csv`. No elimino el
   traqueteo y aumento la velocidad reportada para el mismo objetivo.
2. `5/1`: `svd48_speed_m1_gear_5_to_1_1rpm_2026-07-21.csv`. Fue concluyentemente
   incorrecta: con objetivo de solo 1 RPM oscilo entre aproximadamente `-16` y
   `+13.5 RPM`, genero fault `0x00000800` y el controlador dejo de responder por
   RS485.

Por tanto, la relacion valida para este motor es **driving/driven `1/5`**, no
`5/1`. El ultimo valor guardado antes de la prueba fue `1/5`; el reinicio
electrico debe restaurarlo.

## Estado al cerrar esta etapa

- El ESP sigue accesible por LAN.
- El SVD48 no responde por RS485 despues del fault de la prueba `5/1`.
- El ESP reporta `FAULT`, un comando logico aun activo y repetidos intentos de
  parada con timeout porque el SVD48 no responde.
- Se requiere reinicio electrico completo del SVD48. Al volver, se deben leer
  primero `0x5030/31/34/35`, `0x5300/01`, `0x5304/05`, `0x5410/11` y
  `0x5420..23` antes de cualquier movimiento.

## Conclusiones provisionales

1. El problema no es una inversion simple de `5:1` a `1:5`; `5/1` empeora
   drasticamente el control y dispara una falla.
2. El limite de 15 RPM del ESP funciona, pero no demuestra por si solo la
   velocidad mecanica de la rueda. Hace falta contar vueltas durante un intervalo
   conocido o usar tacometro/video para calibrar comando, telemetria y posicion.
3. Las calibraciones Hall automaticas siguen sin terminar satisfactoriamente.
   Una tabla aparentemente regular no equivale a calibracion aprobada.
4. El consumo sin carga, el traqueteo y la oscilacion indican que no se debe subir
   de 5 A durante el siguiente diagnostico.
5. El firmware interpreta actualmente la palabra cruda de velocidad como RPM en
   algunos caminos tipados. Debe corregirse a `0.1 RPM` antes de usar esa
   telemetria para decisiones de seguridad o UI.

## Restoration requested by operator

The operator stopped the experiment and requested restoration to the original
pre-parameterization capture summarized in `SVD48_ID2_CURRENT_STATE_SUMMARY.md`.
An initial restoration used the wrong, later snapshot and was superseded. No
further movement was performed during either restoration step.

Final values restored from the correct raw inventory include:

- wheel diameter 100 mm;
- original `Lq/Ld/Rs` words;
- 24/24 pole pairs;
- maximum speed 100/100 RPM;
- maximum current 30/30 A;
- acceleration 45/45 and deceleration 40/40 RPM/s;
- M1 Hall table `[0, 44, 103, 164, 224, 283, 345, 0]`.

M2 remains `[0, 40, 99, 161, 220, 280, 343, 0]`; the original table was
`[0, 44, 103, 164, 224, 283, 346, 0]`, but software `0x0131` rejects writes to
these registers. The original inventory did not read the later-discovered gear
registers, so current gear `1/5` cannot be claimed as restored to its original
value. Save register `0x3100=1` was acknowledged, and command, run state, speed
and errors were all zero immediately beforehand.

See `SVD48_RESTORE_TO_INITIAL_2026-07-21.md` for the complete before/after
evidence and the distinction between restored settings and validated settings.
