# Lógica del Firmware Explicada para Humanos

Fecha de última revisión: 2026-07-25

Este documento explica qué hace el firmware sin asumir conocimientos de C/C++. Separa cuidadosamente el comportamiento que existe hoy del comportamiento objetivo descrito en los planes.

## Primero: Este Proyecto es Principalmente C

Aunque ESP-IDF puede enlazar librerías C++, los archivos principales de este repositorio son `.c` y usan C. Para entender la lógica no necesitas memorizar sintaxis. Conviene leer cada función como:

```text
ENTRADAS -> VALIDACIONES -> ACCIONES -> RESULTADO
```

Ejemplo mental:

```text
robot_control_set_motor_speed(motor, rpm)
    recibe motor y velocidad
    valida que el motor exista
    escribe la nueva velocidad en el SVD48
    envía START
    si algo falla intenta detener ese motor
    devuelve OK o ERROR
```

## Mapa Mental del Sistema

```text
PC / navegador / control RC
           |
           v
   comandos y autoridad
           |
           v
   seguridad del robot
           |
           v
    robot_control
      |          |
      v          v
  servos      driver SVD48
                  |
                  v
                RS485
                  |
                  v
       controladores y motores
```

Wi-Fi, OTA y mantenimiento LAN corren como servicios secundarios. No deberían calcular movimiento ni apoderarse del bus de seguridad. El nuevo `control_lan` compila, pero está apagado en `main` hasta que exista una ruta segura completa.

## Qué Sucede al Encender Hoy

La entrada está en `main/main.c` dentro de `app_main()`.

Pseudocódigo simplificado:

```text
mostrar versión del firmware
consultar si este firmware está pendiente de validación OTA

inicializar NVS
inicializar config_manager

intentar iniciar Wi-Fi
si Wi-Fi existe:
    intentar iniciar OTA manager

crear driver SVD48 con:
    UART2
    TX=GPIO17
    RX=GPIO16
    IDs de controlador 1 y 2
    timeout=100ms

crear robot_control con:
    geometría fija RAFA 1.60m x 0.70m
    4 motores
    máximo temporal 15 RPM
    servos deshabilitados

enviar STOP ALL de mejor esfuerzo al arrancar

iniciar polling SVD48

intentar iniciar receptor PPM de 10 canales en GPIO14
iniciar robot_safety
iniciar ota_announce
iniciar serial_gateway
iniciar maintenance_lan

si era una imagen OTA nueva:
    marcar firmware como válido después del self-test actual

iniciar tareas de Wi-Fi/OTA/LAN de baja prioridad

para siempre:
    dormir 5 segundos
```

El `while` de `main` no controla el robot. Las tareas FreeRTOS hacen el trabajo en paralelo.

## Tareas Importantes Hoy

| Tarea | Responsabilidad | Prioridad aproximada |
| --- | --- | ---: |
| `robot_safety` | observar RC/fallos y solicitar stop | 9 |
| `svd48_poll` | leer telemetría RS485 | 8 |
| `serial_gateway` | recibir comandos USB | 6 |
| `ibus_receiver` | recibir control RC | 5 |
| `gateway_stream` | imprimir telemetría | 4 |
| Wi-Fi/LAN/OTA | mantenimiento | 1-3 |

Una prioridad alta ayuda a que una tarea corra antes, pero no puede adelantar mágicamente un `STOP` si el mutex RS485 está ocupado esperando un timeout. Por eso el plan incluye un scheduler de bus con clase de emergencia.

## Qué Hace `robot_safety` Hoy

Pseudocódigo fiel al código actual:

```text
cada 20ms:
    consultar estado del receptor RC

    si alguna vez vimos una trama RC válida:
        y ahora está inválida por al menos 150ms:
            rc_loss = true

    revisar motores:
        ignorar motor si está offline o stale
        si está online/fresh y error_code != 0:
            motor_fault = true

    si rc_loss o motor_fault:
        cada 500ms:
            pedir STOP ALL
```

Limitaciones importantes:

- existe el modelo/servicio de `ARMED/DISARMED/FAULTED`, pero la ruta actual de movimiento no lo instancia ni lo consulta;
- el fallo no queda enclavado;
- si nunca llegó una trama RC, no se considera pérdida;
- un motor offline/stale se ignora en vez de bloquear movimiento;
- después de `STOP`, otra API todavía puede enviar movimiento;
- no hay autoridad ni lease/dead-man de comandos;
- no se conoce si el stop llegó físicamente al controlador.

## Qué Significa `Stale`

`stale` significa: "este valor fue válido, pero es demasiado antiguo para confiar en él".

No significa cero. No significa motor detenido. No significa controlador sano.

Ejemplo:

```text
última lectura válida raw_speed_d10 = 500 (aprox. 50.0 RPM)
se desconecta RS485
después de 1 segundo:
    el raw guardado puede seguir diciendo 500
    STALE = 1
```

La representación correcta es:

```text
raw_d10=500, rpm=50.0, freshness=STALE, age=1200ms
```

No:

```text
valor=0, healthy=true
```

La política objetivo será:

```text
si un dato crítico está stale mientras ARMED:
    FAULTED
    inhibir nuevas consignas
    solicitar emergency stop
```

## Cómo Habla el ESP32 con el SVD48

El ESP manda bytes por RS485. Una lectura contiene, de forma simplificada:

```text
[ID controlador] [función 0x03] [registro] [cantidad] [CRC]
```

Una escritura de un registro:

```text
[ID] [función 0x06] [registro] [valor] [CRC]
```

Una escritura de varios registros:

```text
[ID] [función 0x10] [registro inicial] [cantidad] [bytes] [valores] [CRC]
```

El CRC permite detectar bytes corruptos. El ID evita aceptar la respuesta de otro controlador. La función y la cantidad permiten detectar una respuesta que no corresponde a lo pedido.

## Qué es una Lectura Raw

El comando actual:

```text
READ_REG 2 0x5018 1
```

puede devolver algo como:

```text
R0:0x0018/24
```

Esto solo dice que en una dirección hay 16 bits cuyo número decimal es 24. No explica:

- qué parámetro es;
- si es entero con signo, sin signo o parte de un float;
- qué unidad tiene;
- qué rango es seguro;
- si se puede escribir;
- si cambia M1, M2 o toda la placa;
- si queda guardado después de apagar;
- si ese registro existe en esta versión del controlador.

## Qué es una Lectura Tipada

"Tipada" significa que el programa conoce el significado del valor, no solamente sus bytes.

Ejemplo:

```text
key: motor.m1.pole_pairs
nombre: Pares de polos M1
registro: 0x5018
tipo: u16 (entero positivo de 16 bits)
valor: 24
unidad: pole_pairs
rango permitido: 1..128
acceso: lectura/escritura
scope: controlador 2, canal M1
freshness: fresh
confidence: observed
persistencia: por verificar
```

### Analogía

Una lectura raw es recibir una caja que dice `0018`.

Una lectura tipada es recibir una ficha que dice:

```text
Contenido: 24 pares de polos
Para: motor M1
Valores permitidos: 1 a 128
Medido hace: 20ms
Se puede escribir: sí, bajo mantenimiento
```

## Por Qué Leer Tipado Antes de Escribir

Porque una escritura segura necesita responder estas preguntas:

1. ¿Qué valor tiene ahora?
2. ¿Cómo se decodifica?
3. ¿El nuevo valor cabe en el tipo correcto?
4. ¿Está dentro del rango del producto y del límite de Botfarms?
5. ¿Cuántos registros ocupa?
6. ¿En qué orden van las palabras?
7. ¿Se permite escribir en este estado?
8. ¿Cómo verificamos que quedó aplicado?

Pseudocódigo:

```text
descriptor = catálogo.buscar("motor.m1.pole_pairs")

si descriptor no existe:
    rechazar UNKNOWN_PARAMETER

valor_anterior = leer_y_decodificar(descriptor)

si valor_anterior está stale/unknown:
    rechazar BASELINE_NOT_FRESH

si nuevo_valor no cumple descriptor.rango:
    rechazar OUT_OF_RANGE

bytes = codificar(nuevo_valor, descriptor.tipo)
escribir bytes
valor_leido = leer_y_decodificar(descriptor)

si valor_leido != nuevo_valor:
    fallar READBACK_MISMATCH
```

### Ventajas para Ti como Desarrollador

- La UI muestra nombres y unidades, no hexadecimal obligatorio.
- Evita escribir `65535` cuando se quería `-1`.
- Evita intercambiar las dos palabras de un float.
- Permite inputs, selects y límites apropiados automáticamente.
- La misma definición funciona por USB y Wi-Fi.
- Permite comparar configuración esperada contra configuración real.
- Hace posible dry run, readback, audit y rollback.
- Permite ocultar o bloquear parámetros no soportados por una versión del SVD48.
- Los tests pueden usar valores humanos y comprobar bytes exactos.

No es una complicación de C++; es un diccionario formal entre "parámetro humano" y "bytes del controlador".

## Qué Hace una Escritura Hoy

Pseudocódigo de `WRITE_REG`:

```text
recibir drive_id, registro, valor y CONFIRM
si el registro es una consigna/START/STOP conocida:
    rechazar WRITE_REG_ACTUATION_BLOCKED
si el comando recordado o una RPM fresca indica movimiento:
    rechazar WRITE_REG_ROBOT_NOT_STOPPED
leer y guardar old_value
llamar robot_control_write_svd48_register
llamar svd48_write_register_by_id
construir frame 0x06
enviar y esperar eco
si el write falla despues de transmitir:
    devolver OUTCOME:UNKNOWN
leer el registro otra vez
si readback == valor:
    devolver VERIFIED:1 con old/value/readback
si no:
    devolver error sin reintento ciego
```

`WRITE_REGS` hace el mismo flujo para hasta 8 words contiguos con FC `0x10` y
un solo intento. USB y `maintenance_lan` comparten el dispatcher. Esto ya permite
un formulario web de banco, pero sigue sin sesión exclusiva, catálogo completo,
hard ceilings, deduplicación, rollback ni persistencia verificada. Nunca se debe
reintentar un resultado `UNKNOWN`/`ACKED_UNVERIFIED` sin leer primero.

Build 19 contiene además una excepción temporal para diagnóstico con ruedas
elevadas: `maintenance_lan` acepta `SET_SPEED n rpm` y `STOP n|ALL`. La consigna
queda limitada a `+/-15 RPM`, pero **no vence** si desaparece el cliente o el
Wi-Fi. No existe dead-man, TTL ni arbitraje en esa ruta; una consigna exitosa
puede seguir activa hasta recibir otra consigna/stop o hasta que actúe otra
condición de seguridad. Por eso no es un canal de movimiento productivo.

## Movimiento Actual Simplificado

Para `SET_SPEED`:

```text
validar motor
rechazar si |rpm| > 15
limitar nuevamente al máximo de robot_control
escribir nueva RPM
enviar START
si falla:
    intentar stop de ese motor
si funciona:
    recordar comando
```

Para `MOVE_VEL`:

```text
calcular 4 RPM y 4 ángulos de servo
limitar RPM máxima
aplicar ángulos uno por uno
escribir RPM una por una
enviar START a motores uno por uno
si cualquier paso falla:
    pedir STOP ALL
```

Esto mejora el problema de una consigna residual, pero no es atómico: mientras se actualizan cuatro dispositivos físicos puede existir un estado parcial. El objetivo es detectarlo, detener e inhibir, no prometer atomicidad imposible.

## Nuevo Contrato: El Robot Es un JSON

El firmware objetivo no debe asumir "cuatro motores" ni "dos controladores".
Recibe un JSON autocontenido parecido conceptualmente a esto:

```text
robot
    placa y pines
    buses RS485 y receptores RC
    controladores[]
        direccion Modbus
        canales M1/M2 enlazados a motores por ID
        parametros SVD48 deseados por canal
    motores[]
        posicion fisica
        controlador/canal
        radio, transmision, signo y limites
    servos[]
        ruedas que dirige, GPIO y calibracion
    ruedas_pasivas[]
    geometria
    modo_cinematico
    fuentes_de_control
    seguridad y servicios
```

Ejemplos que usan el mismo formato:

```text
1 SVD48:
    M1 -> motor izquierdo
    M2 -> motor derecho
    2 ruedas locas
    kinematics = differential

2 SVD48:
    cuatro canales -> cuatro motores
    kinematics elegida por el perfil

4 SVD48:
    un canal usado de cada controlador -> cuatro motores
    el mapping cambia, el schema no
```

El ESP valida el JSON y construye una copia C eficiente e inmutable para el loop.
Esa copia no reemplaza al JSON como fuente de verdad. El JSON exacto se conserva
en slots active/staged/known-good y se identifica por revision/hash.

Un archivo `draft` puede describir la topologia sin estar listo para mover. Si
`activation_allowed` es falso, el firmware debe rechazar su activacion aunque su
sintaxis sea correcta. Asi una medida placeholder no se convierte en un parametro
real por accidente.

## Nuevo Contrato: RC, LAN y Bluetooth Simultaneos

Estado build 13: el árbitro puro ya implementa y prueba esta prioridad, TTL,
dead-man, streams retirados, epoch y no-fallback. Todavía no recibe datos reales
de RC/LAN/Bluetooth ni controla `robot_control`.

Las tres entradas se siguen leyendo, pero ninguna escribe directamente a los
motores:

```text
RC ---------> mailbox RC -----------+
LAN --------> mailbox LAN ----------+--> arbitro --> safety --> kinematics --> I/O
Bluetooth --> mailbox Bluetooth ----+
```

El diseño objetivo mantiene `maintenance_lan` como diagnóstico/configuración. Un
`control_lan` separado recibe solamente comandos compactos con stream, secuencia
y TTL, y los publica en el mailbox LAN. Ese diseño todavía no está activo. La
excepción directa `SET_SPEED` de build 19 contradice temporalmente esta frontera
y debe retirarse o quedar detrás del coordinador antes de cualquier prueba de
piso/producto (`SAFE-011`, ADR-0004).

En cada ciclo rapido:

```text
copiar ultimo dato de cada mailbox sin esperar I/O

si RC esta fresh y valido:
    elegir RC
si no, si LAN tiene comando fresh:
    elegir LAN
si no, si Bluetooth tiene comando fresh:
    elegir Bluetooth
si no:
    elegir NONE
```

RC neutral o con dead-man liberado sigue ganando y produce cero. Esto permite que
un operador con RC detenga/bloquee un comando de red. Si RC fue configurado como
`optional` y no existe, LAN o Bluetooth pueden operar; la politica de presencia
tambien vive en el JSON.

Cuando cambia la fuente:

```text
detener primero
crear un nuevo epoch de autoridad
invalidar comandos recibidos antes del cambio
esperar un comando nuevo de la fuente seleccionada
```

Cuando la fuente activa vence mientras habia movimiento:

```text
STOP inmediato
FAULTED
NO usar automaticamente el comando viejo de otra fuente
```

La prioridad `RC > LAN > Bluetooth` es una regla de autoridad, no el numero de
prioridad FreeRTOS de cada tarea. Wi-Fi/JSON siguen en tareas bajas y nunca
bloquean el loop de safety/control.

## Máquina de Estados Objetivo

Estado de implementacion al 2026-07-19: la logica pura y un servicio mutex de transiciones/snapshots ya existen y compilan. El servicio no ofrece un "permiso" separado del write porque eso crea una carrera check/use; el gate real debe vivir dentro del coordinador que sea dueño único de los actuadores. Todavia no está conectado a `main`, `robot_control`, `robot_safety` ni a los comandos USB/LAN. Por eso el firmware actual aun no puede afirmar que todo movimiento pase por este gate.

Pseudocódigo conceptual:

```text
estado = BOOTING

al terminar self-test:
    estado = DISARMED

cuando llega ARM_REQUEST:
    si existe una fuente elegible segun el JSON
       y se cumplen sus condiciones de arm/dead-man
       y controladores fresh
       y perfil válido
       y no faults
       y no maintenance/OTA:
           crear epoch de autoridad + TTL
           estado = ARMED
    si no:
           rechazar con razones

cuando llega un comando de movimiento:
    si estado != ARMED:
        rechazar
    si source/epoch != autoridad seleccionada:
        rechazar
    si TTL expiró:
        FAULTED + emergency stop
    si todo es válido:
        calcular y aplicar targets

cuando llega ACTIVE_SOURCE_LOSS / STALE / MOTOR_FAULT / E_STOP:
    guardar fault latch
    estado = FAULTED
    inhibir cualquier nueva salida
    solicitar emergency stop

cuando llega FAULT_ACK:
    si la causa ya desapareció:
        estado = DISARMED
    si no:
        continuar FAULTED
```

## Flujo Objetivo de Escritura: Primero Backend, Luego Frontend

El primer cliente sera CLI/backend por USB y LAN. Cuando ese contrato este probado,
el frontend ejecutara exactamente la misma secuencia:

```text
cliente envia request_id + change set tipado
LAN valida el maintenance token existente
ESP verifica que está DISARMED y seguro
ESP entra MAINTENANCE y crea un job exclusivo

cliente pide schema y valores
ESP lee SVD48 y devuelve parámetros tipados

cliente prepara cambios
NO se escribe todavía

cliente llama VALIDATE
ESP valida estado, exclusividad, rango, tipo, soporte y baseline
devuelve un dry run

cliente revisa old -> new y llama APPLY
ESP vuelve a validar todo
ESP pide stop/cero
ESP escribe una sola vez
ESP lee nuevamente
ESP compara readback
ESP guarda resultado por request/job ID

si se pierde Wi-Fi:
    el job sigue siendo propiedad del ESP
    al reconectar se consulta por job_id
    nunca se supone success por desaparecer el browser
```

## El Caso Difícil: Se Perdió la Respuesta del Write

```text
ESP envía: escribir 24 pares de polos
SVD48 puede haber aplicado el valor
pero la respuesta RS485 se pierde
```

No sabemos si escribió. Reenviar podría repetir una acción no idempotente.

Comportamiento correcto:

```text
marcar OUTCOME_UNKNOWN
no reenviar a ciegas
intentar leer el registro

si readback == requested:
    aplicado, pero response se perdió
si readback == old:
    no aplicado
si no se puede leer:
    recovery required + FAULTED/MAINTENANCE bloqueado
```

## Configuración de Pines Objetivo

Hoy los pines están en macros:

```text
RS485 TX=17 RX=16
servos=4,5,6,7
i-BUS RX=18
```

No se cambiarán inmediatamente al editar la UI.

Pseudocódigo:

```text
usuario/backend edita las secciones board/io/servos del robot JSON completo
validar GPIOs reservados, duplicados y capacidades
guardar slot STAGED y comprobar JSON/schema/SHA-256
usuario confirma reinicio

al arrancar:
    cargar staged como active_pending
    iniciar UART/LEDC sin habilitar movimiento
    ejecutar self-test

    si funciona:
        quedar DISARMED
        permitir promover a known_good
    si falla:
        siguiente boot usa known_good/factory
        quedar DISARMED o FAULTED
```

UART y LEDC no se reconfiguran en caliente durante el MVP. Los pines de flash, PSRAM, recuperación USB y E-stop no serán editables.

## Cómo Leer un Archivo C sin Conocer C

1. Empieza por el `.h`: muestra qué ofrece el componente.
2. Busca la función concreta con `rg "nombre_funcion"`.
3. Lee primero validaciones y llamadas a otros componentes.
4. Ignora temporalmente detalles de `malloc`, mutex y logs.
5. Escribe al lado el pseudocódigo `entrada -> decisiones -> salida`.
6. Verifica qué sucede en cada `return` de error.
7. Busca quién llama la función; una función segura puede ser rodeada por un caller inseguro.

Ejemplo:

```c
if (!handle || motor >= SVD48_MOTOR_COUNT) {
    return ESP_ERR_INVALID_ARG;
}
```

se lee como:

```text
si el robot no existe o el motor está fuera de rango:
    rechazar argumentos
```

## Glosario Corto

- **State**: modo operativo exclusivo actual.
- **Fault latch**: fallo recordado que necesita reconocimiento explícito.
- **Inhibit**: razón que impide armar/configurar.
- **Authority**: fuente que tiene permiso temporal para mover.
- **Lease**: permiso con vencimiento; evita comandos eternos.
- **Dead-man**: señal que debe mantenerse activa para permitir movimiento.
- **Fresh**: dato reciente dentro del límite temporal.
- **Stale**: dato antes válido pero demasiado viejo.
- **Typed**: bytes interpretados con nombre, tipo, unidad y reglas.
- **Baseline**: valores leídos justo antes de preparar cambios.
- **Change set**: conjunto revisable de cambios old -> new.
- **Readback**: lectura posterior para comprobar una escritura.
- **Outcome unknown**: se transmitió una operación, pero no sabemos si el dispositivo la aplicó.
- **Idempotent**: repetir la misma operación no produce una segunda mutación.
- **HIL**: prueba con hardware real conectado.
- **Fail-stop**: ante fallo parcial, inhibir y solicitar parada.

## Documentos para Profundizar

- Estados, writes y pines: `docs/process/05_SAFE_CONFIGURATION_WRITE_PLAN.md`.
- QA/fallos simulados: `docs/process/06_SIMULATED_QA_FAULT_INJECTION_PLAN.md`.
- Perfiles/kinematics/safety: `docs/process/02_ROBOT_PROFILES_KINEMATICS_SAFETY.md`.
- Registros SVD48: `docs/process/01_SVD48_REGISTER_COVERAGE.md`.
- Pruebas antes de piso: `docs/process/04_OFF_GROUND_TEST_MATRIX.md`.
