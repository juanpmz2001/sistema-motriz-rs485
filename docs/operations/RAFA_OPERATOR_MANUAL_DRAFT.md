# Rafa — Manual de operación por radio FS/PPM

> **BORRADOR CONTROLADO PARA CAPACITACIÓN. NO ES AUTORIZACIÓN DE MOVIMIENTO.**
>
> Este documento solo podrá usarse como manual de campo cuando el responsable de
> operación registre abajo una versión de firmware de producción con perfil
> **rafa**, complete las validaciones físicas pendientes y autorice expresamente su
> uso. No aplica al perfil experimental **rafa_softap_web_joystick_experimental**
> ni a ningún perfil de diagnóstico.

## Control del documento y liberación de operación

| Campo | Registro requerido antes de liberar el manual |
| --- | --- |
| Fecha y lugar de liberación | _Pendiente_ |
| Responsable de operación | _Pendiente_ |
| Firmware: versión / build / SHA | _Pendiente: leer VERSION del robot_ |
| Perfil reportado | Debe ser **rafa** |
| Prueba supervisada de radio | Avance, reversa, ambos giros, parada, pérdida de señal y paro de tracción documentados |
| Pulsador de paro de tracción | Función y latencia física verificadas; identificado como incapaz de detener la motobomba |
| Procedimiento de corte total de energía | _Pendiente: identificar y etiquetar el corte físico de batería_ |
| Fuentes de control durante operación PPM | Solo la radio autorizada; no debe haber Console/LAN activa ni otro operador de control |
| Integridad PPM | Prueba sobre el SHA instalado: exactamente 8 canales aceptados y contadores de tramas inválidas, overflow y rechazo revisados |
| Guarda de correa | Instalada, íntegra e inspeccionada. Sin guarda no hay liberación de bomba ni de aspersión. |
| Identidad TS-28 y disposición hidráulica | _Pendiente: fotografía de placa, marca y variante; relación real de poleas/RPM, manómetro, retorno/bypass y presión/caudal validados para los dos brazos de 36 boquillas._ |
| Motor de bomba y protección eléctrica | _Pendiente: foto de placa WEG, tensión/frecuencia entregada por el inversor, puesta a tierra, protección de sobrecarga, capacidad de arranque y ventilación revisadas por técnico/electricista._ |
| Calibración física CH6 | Izquierda ≈ 50 %, centro ≈ 75 % y derecha ≈ 100 % de la escala de límites; verificar también el efecto en avance y giro |
| Límites aprobados de terreno, pendiente y velocidad | _Pendiente_ |
| Líquidos autorizados, EPP y SDS/FDS | _Pendiente por responsable agrícola/químico_ |

Si cualquiera de esos campos está pendiente, Rafa es material de capacitación o
prueba supervisada; no debe operar como equipo de campo autónomo.

## 1. Propósito y alcance

Rafa es una plataforma móvil de riego/aspersión operada visualmente por una persona
capacitada mediante radio FlySky FS-i6 y señal PPM. No es autónoma. El operador debe
mantener línea de vista, controlar el área de trabajo y detener la operación ante
cualquier comportamiento inesperado.

Este manual enseña la operación exterior del equipo. No autoriza abrir la caja verde
de control, cambiar fusibles, intervenir cableado, ajustar el SVD48/ESP32, modificar
parámetros, ni reparar batería, inversor, motor o bomba. Esas tareas pertenecen a
personal técnico autorizado.

## 2. Conozca a Rafa

### 2.1 Configuración física informada

| Elemento | Descripción de operación |
| --- | --- |
| Envolvente exterior informada | 1.00 m de alto × 0.90 m de largo × 1.60 m de ancho. Verificar y señalizar esta envolvente antes de fijar distancias de seguridad. |
| Tracción | Dos ruedas hub motrices de 16 pulgadas. |
| Apoyo | Dos ruedas locas traseras de 20 cm de diámetro. |
| Tanque | Capacidad nominal de 100 L. La carga modifica la estabilidad y exige reducir velocidad. |
| Aspersión | Dos brazos con 18 boquillas cada uno: 36 boquillas en total. |
| Energía de tracción | Batería MUST LiFePO4 de 48 V / 100 Ah, dentro del conjunto técnico. |
| Bomba | Referencia declarada: motobomba TS-28 por correa y regulador manual de presión. La guía del fabricante DIAMOND/Tanong es una referencia provisional hasta confirmar placa y variante física. Los dos brazos y 36 boquillas de Rafa no son la configuración estándar de esa guía; no usar fichas genéricas para fijar presión o caudal. |
| Motor de la bomba | Referencia declarada: motor monofásico WEG de 3 HP / 2,2 kW, 110/220 V, IP21. La ficha de compra informa 1.780 rpm; la placa física, tensión/frecuencia y diagrama del motor prevalecen. El operador no cambia cables, capacitor, protecciones ni ajustes del inversor. |
| Inversor | MUST PV3000 LVHM Series; su interruptor controla el encendido/apagado de la motobomba cuando la batería está encendida. |

**Orientación obligatoria.** Antes de capacitar a un operador, instalar una marca
visible **FRENTE** en Rafa. Para este manual, la parte trasera es la que tiene las dos
ruedas locas; la referencia derecha/izquierda debe leerse estando detrás de Rafa y
mirando hacia su frente.

### 2.2 Caja técnica

La caja verde contiene equipos eléctricos y de control. Para el operador es una zona
de **inspección exterior solamente**:

- mantenerla cerrada, seca y sin objetos encima;
- no abrirla, no puentear fusibles y no reconectar cables;
- no rociarla, lavarla ni dirigir agua hacia la caja, batería, inversor, conectores o
  radio mientras no exista una clasificación de protección documentada;
- retirar a Rafa de servicio si hay olor a quemado, humo, cables sueltos, fusible
  abierto visible o daño de la caja;
- solicitar al técnico designado antes de intentar cualquier corrección.

## 3. Riesgos críticos

### 3.1 Paro de tracción no es paro total

El pulsador rojo/amarillo visible junto al tanque es el **pulsador de paro de
tracción**: su función y latencia física deben verificarse durante la liberación del
equipo. **No detiene la motobomba.**

Si también debe detenerse la aspersión, el operador debe poner el interruptor del
inversor en **OFF**, siempre que sea seguro hacerlo. No acercar manos, ropa, cabello ni
herramientas a la transmisión por correa para intentar detenerla.

El procedimiento de aislamiento total de batería aún debe identificarse, etiquetarse
y validarse en la liberación de este manual. Un paro por software, centrar el joystick
o el pulsador de tracción no sustituyen ese corte físico total.

### 3.2 Correa, motor y bomba

**Está prohibido operar la motobomba si la correa o sus poleas están expuestas, si
falta su guarda fija o si la guarda está dañada.**

No retirar una guarda, no ajustar tensión de correa, no limpiar ni revisar la bomba
con el inversor encendido. Reportar vibración anormal, ruido, rozamiento, olor a
caucho, fuga o correa floja al responsable técnico.

### 3.3 Bomba TS-28 y motor WEG

Esta sección resume en lenguaje de operación las indicaciones consultadas para una
TS-28 DIAMOND/Tanong de transmisión por correa y regulador manual, y para la familia
de motor monofásico WEG de referencia. **Solo aplica después de cotejar las placas
físicas con Rafa.** No reemplaza los manuales originales ni autoriza mantenimiento
eléctrico, hidráulico o mecánico.

- La TS-28 consultada requiere una correa y poleas alineadas, sin deslizamiento, y
  mangueras de succión, retorno y descarga firmemente conectadas, sin entrada de aire.
  El filtro de succión debe estar limpio y el manómetro debe ser legible.
- La presión se regula en el conjunto de la bomba después de que su giro sea normal.
  En Rafa **no hay todavía una presión ni caudal aprobados**: dependen de la placa,
  relación de poleas, RPM real, retorno/bypass, producto y las 36 boquillas. El
  operador no usa una cifra tomada de internet ni cierra ambas salidas mientras no
  exista un procedimiento marcado y validado para el retorno/bypass.
- El motor IP21 de referencia no es una envolvente para lluvia, lavado, polvo fino ni
  niebla de aspersión. Mantener secos, ventilados y fuera del chorro el motor, sus
  terminales, el inversor, batería, cajas y conectores.
- Si el motor no arranca, se detiene inesperadamente, se calienta en exceso, produce
  olor anormal, ruido o vibración, poner el inversor en **OFF** cuando sea seguro,
  mantener distancia de la transmisión y reportar el evento. No retirar ni intervenir
  la guarda, ni intentar reiniciar, cambiar tensión, reconectar cables o revisar
  capacitores.
- Cableado, puesta a tierra, cambio 110/220 V, protecciones, capacidad de salida y
  arranque del inversor, y cualquier ajuste de correa pertenecen al técnico o
  electricista autorizado. La placa física del motor prevalece sobre el título de una
  ficha comercial.

### 3.4 Riego, aspersión y productos

- Usar únicamente líquidos, concentraciones y equipos de protección personal (EPP)
  aprobados para la labor y documentados en la SDS/FDS del producto.
- Mantener personas, animales, cultivos no objetivo y fuentes de agua fuera de la
  zona de aplicación conforme al procedimiento agrícola aplicable.
- No abrir, cerrar ni regular válvulas mientras Rafa está desplazándose.
- Suspender la aspersión ante una fuga, boquilla desprendida, manguera dañada o
  deriva no controlada.

La presión, caudal, cebado, compatibilidad química, drenaje y limpieza final deben
incorporarse desde la placa y manual de la TS-28 realmente instalada, más el
procedimiento agrícola aprobado; no se deben inferir de este documento.

### 3.5 Movimiento del vehículo

- Nadie debe estar delante, detrás, debajo o entre Rafa y un obstáculo durante la
  operación.
- Mantener siempre línea de vista y un camino libre; no conducir desde el lado ciego
  de una pendiente, muro, cultivo denso o vehículo.
- No operar con una rueda floja, un caster trabado, una manguera arrastrando, una
  fuga, cable expuesto o carga sin asegurar.
- Los límites de pendiente, terreno, velocidad máxima y distancia de exclusión deben
  estar aprobados en la tabla de liberación antes de uso de campo.

## 4. Radio FlySky FS-i6

### 4.1 Controles autorizados

| Control físico | Función para Rafa | Regla de uso |
| --- | --- | --- |
| Joystick izquierdo, arriba/abajo | Avance / reversa | Iniciar siempre desde centro y mover suavemente. |
| Joystick izquierdo, izquierda/derecha | Giro a la izquierda / derecha | Girar despacio; evitar giro brusco con tanque cargado. |
| Switch CH5 sobre el joystick | Arriba: radio habilitada para conducir. Abajo: tracción RC deshabilitada. | Encender y apagar la conducción solo con el joystick centrado. |
| Control de velocidad CH6 | Escala de velocidad | Extremo máximo a la izquierda: escala de inicio mínima. No equivale a una parada. |

El firmware requiere que los mandos de avance/reversa y giro permanezcan centrados
después de habilitar CH5 antes de aceptar movimiento. Por ello, nunca cambiar CH5 con
el joystick desplazado.

CH5 abajo termina la fuente de movimiento de la radio, pero **no es un corte total de
energía** ni prueba que otra fuente de control de red esté desarmada. Antes de acercarse
a Rafa: joystick centrado, CH5 abajo, verificar inmovilidad y aplicar el procedimiento
de aislamiento físico aprobado cuando corresponda. La liberación de operación PPM debe
mantener Console/LAN fuera de uso durante el trabajo de campo.

### 4.2 Política de velocidad

1. Iniciar cada operación con CH6 completamente a la izquierda.
2. Trabajar en la escala baja hasta confirmar terreno, espacio, visibilidad,
   estabilidad y respuesta de Rafa.
3. No aumentar por encima de la mitad de la escala salvo en condiciones de extrema
   seguridad, con operador entrenado y terreno conocido.
4. No operar por encima de la mitad de la escala con el tanque lleno.

Los puntos anteriores son la política conservadora indicada por el propietario; no
constituyen una calificación física de velocidad, pendiente o terreno.

La escala mínima configurada no representa velocidad cero. Para detener Rafa se debe
centrar el joystick y pasar CH5 a la posición inferior de tracción RC deshabilitada,
o usar el pulsador de paro de tracción si es una emergencia. La escala mínima actual
sigue siendo el 50 % del límite configurado, no una velocidad de arrastre garantizada
ni velocidad cero; su posición física izquierda y la velocidad real deben verificarse
en la liberación del equipo.

## 5. Lista de comprobación antes de encender

El operador marca todos los puntos antes de activar la radio o la bomba:

- [ ] Área despejada; no hay personas, animales ni obstáculos en la trayectoria o
      zona de aspersión.
- [ ] Terreno, pendiente y carga dentro de los límites aprobados.
- [ ] Tanque cerrado, carga asegurada y sin fugas.
- [ ] Brazos, boquillas, mangueras, filtros y válvulas inspeccionados; no hay piezas
      sueltas ni mangueras que puedan quedar bajo las ruedas.
- [ ] Mangueras de succión, retorno/bypass y descarga firmemente conectadas; filtro
      de succión sin obstrucción visible, manómetro legible y sin fuga o entrada de
      aire visible.
- [ ] Posición segura y marcada del retorno/bypass confirmada. No se usará una
      presión objetivo ni se cerrarán ambas salidas sin esa validación.
- [ ] Ruedas hub y casters giran libremente; no hay daño visible.
- [ ] Guarda de correa instalada e íntegra; correa y poleas no están accesibles.
- [ ] Caja técnica cerrada, sin olor anormal, cables sueltos ni humedad visible.
- [ ] Motor IP21 de referencia, inversor, cajas y conectores secos; ventilación libre
      y ninguna boquilla o fuga puede dirigir agua hacia ellos.
- [ ] Pulsador rojo/amarillo de paro de tracción accesible y sin obstrucciones.
- [ ] Radio FS-i6 cargada; joystick izquierdo centrado; CH5 abajo; CH6 al extremo
      izquierdo.
- [ ] Ninguna Console, control LAN u otra persona está autorizada para controlar Rafa
      durante esta operación PPM.
- [ ] Motobomba apagada mediante el interruptor del inversor.
- [ ] Líquido, EPP y procedimiento de aspersión aprobados para la tarea.

Si un punto falla, no operar. Etiquetar Rafa como fuera de servicio y comunicarlo al
responsable técnico u operativo.

## 6. Encendido y conducción por radio

### 6.1 Preparación

1. Completar la lista de comprobación.
2. Confirmar que CH5 está abajo, CH6 está al extremo izquierdo y el joystick
   izquierdo está centrado. CH5 abajo no sustituye el aislamiento físico total.
3. Encender la radio FS-i6.
4. Energizar Rafa únicamente mediante el procedimiento físico aprobado y etiquetado.
   No abrir las cajas verdes ni improvisar conexiones.
5. Confirmar visualmente que Rafa permanece inmóvil antes de habilitar conducción.

### 6.2 Habilitar la radio

1. Mantener el joystick izquierdo centrado.
2. Mover CH5 hacia arriba: **radio habilitada para conducir**.
3. Mantener los dos ejes del joystick centrados durante al menos un segundo y hasta
   completar la comprobación de neutral. No intentar salir de inmediato.
4. Con el área despejada, aplicar una entrada pequeña de avance o reversa y observar
   la respuesta antes de seguir.

### 6.3 Conducir

- Joystick hacia arriba: avance.
- Joystick hacia abajo: reversa.
- Joystick a la izquierda: giro a la izquierda.
- Joystick a la derecha: giro a la derecha.
- Devolver el joystick al centro para solicitar velocidad cero.

Con el joystick centrado Rafa puede seguir armado en retención a velocidad cero. Para
terminar la fuente RC, además poner CH5 abajo y comprobar que dejó de moverse. No
acercarse ni intervenir Rafa hasta confirmar que no existe otra fuente de control y,
cuando corresponda, aplicar el aislamiento físico aprobado.

Evitar maniobras bruscas, cambios súbitos de avance a reversa y giros cerrados con el
tanque cargado. Reducir la escala CH6 antes de entrar a zonas estrechas, superficies
irregulares o áreas con visibilidad limitada.

### 6.4 Detener Rafa

| Situación | Acción |
| --- | --- |
| Detención normal | Centrar joystick → CH5 abajo → verificar inmovilidad. CH5 abajo no es corte total de energía. |
| Riesgo inmediato de movimiento | Pulsar el paro rojo/amarillo de tracción → CH5 abajo → mantenerse fuera de ruedas y correa. No asumir una latencia física hasta que se haya validado. |
| Bomba debe detenerse | Poner el interruptor del inversor en **OFF** cuando sea seguro. El pulsador de tracción no para la bomba. |
| Pérdida de señal o conducta inesperada | No acercarse a Rafa; usar el paro físico de tracción si es necesario, poner CH5 abajo y solicitar ayuda. No confiar solo en una desconexión de radio para detener todos los peligros. |

No reiniciar la conducción después de un paro de emergencia hasta que la causa haya
sido revisada y el responsable autorice el retorno a servicio.

## 7. Operación de la motobomba y aspersión

> **Límite de esta instrucción.** La TS-28 consultada usa transmisión por correa y regulador manual. Hasta cotejar la placa, la relación de poleas, el retorno/bypass y el manómetro de Rafa, este procedimiento no autoriza una presión, caudal ni posición de válvula específicos.

### 7.1 Preparar el sistema

1. Con Rafa detenido y CH5 abajo, verificar nivel del tanque, tapa, mangueras,
   conexiones, brazos, boquillas y válvulas.
2. Verificar que las mangueras de succión, retorno/bypass y descarga estén sujetas,
   que no haya fuga ni entrada de aire, que el filtro de succión esté limpio y que el
   manómetro sea legible. Si alguno no lo está, no arrancar.
3. Si el visor de aceite de la bomba está identificado por el técnico, confirmar solo
   visualmente que el nivel es el marcado para esa bomba. No rellenar ni cambiar
   aceite; esa tarea pertenece al personal técnico.
4. Confirmar que la guarda de correa está instalada, que nadie está cerca de la
   transmisión y que motor, inversor, cajas y conectores están secos y ventilados.
5. Abrir o cerrar manualmente cada brazo únicamente con Rafa detenido. No cerrar
   ambas salidas con la bomba en marcha mientras no esté etiquetada y comprobada la
   posición segura del retorno/bypass.
6. Verificar que la batería esté encendida antes de usar el inversor y que el líquido,
   EPP y plan de aplicación sean los aprobados para la tarea.

### 7.2 Arrancar y detener la bomba


1. Despejar el área y comprobar nuevamente que no hay manos, ropa, cabello ni
   herramientas cerca de la correa o poleas.
2. **Arranque:** usar el switch **ON** del inversor MUST para encender la motobomba.
   Desde una posición segura, observar giro normal, retorno o flujo esperado y lectura
   estable del manómetro. No regular presión hasta disponer de un valor aprobado y
   marcado para Rafa.
3. **Detención:** usar el switch **OFF** del inversor MUST. Esperar la parada total
   antes de acercarse; liberar presión, drenar o intervenir solo mediante el
   procedimiento técnico/hidráulico aprobado.
4. La bomba no se inicia ni se detiene con CH5, el joystick ni el pulsador de paro de
   tracción.

No poner en marcha la bomba si no se ha confirmado el cebado, nivel mínimo y ruta de
retorno/bypass de la TS-28 realmente instalada. Ante ausencia de succión, presión
inestable, lectura anormal o fuga, poner el inversor en **OFF** y reportar; no intentar
corregirlo con la transmisión energizada.

### 7.3 Durante la aspersión

- Vigilar las dos salidas, las mangueras, el patrón de boquillas y la deriva.
- Detener la bomba con el inversor ante fuga, cambio de patrón, pérdida de succión,
  pulsación o lectura anormal de presión, ruido, vibración, olor o presencia de
  personas en la zona de riesgo.
- Ajustar válvulas y brazos solo con Rafa detenido y sin riesgo de contacto con la
  correa o ruedas.
- Nunca usar la aspersión para aplicar un producto sin su SDS/FDS, EPP y procedimiento
  agrícola autorizado.
- No dirigir la aspersión, agua de lavado ni una fuga hacia el motor IP21, sus
  terminales, el inversor, batería, cajas, conectores ni radio.

### 7.4 Después de la aspersión

- Cuando el producto, la SDS/FDS y el plan agrícola lo permitan, circular agua limpia
  por el sistema durante aproximadamente cinco minutos, como indica el manual
  consultado de la TS-28. Recoger y disponer el agua de lavado según el producto;
  nunca tratarla como agua potable.
- Con la bomba detenida y la presión tratada por el procedimiento aprobado, limpiar y
  secar externamente mangueras y revisar fugas. No abrir la bomba ni retirar la guarda.
- El manual consultado indica regresar el ajuste de presión hacia sentido antihorario
  después de usarla. En Rafa ese ajuste queda reservado al técnico hasta que su punto
  seguro y su etiqueta sean validados.
- Registrar horas de operación, fugas, ruido, vibración, pérdida de rendimiento o
  disparo del inversor. El mantenimiento de aceite, válvulas, sellos, engrase y correa
  debe realizarlo el técnico según la placa/manual de la bomba física.
- Una vez que la placa confirme que corresponde a la TS-28 consultada, el técnico
  puede usar como referencia de mantenimiento del fabricante: aceite lubricante
  #30/#40 hasta el nivel indicado por el visor, primer cambio a las 50 horas y luego
  cada 100 horas, y atención de las tres copas de grasa según ese manual. El operador
  solo registra horas y anomalías; no abre ni interviene la bomba.

## 8. Fin de la operación

1. Llevar Rafa a un lugar seguro y nivelado.
2. Centrar joystick, poner CH5 abajo y verificar que la tracción esté detenida. No
   asumir que CH5 abajo es un corte total de energía.
3. Poner el inversor en **OFF** si la motobomba está encendida.
4. Cerrar los brazos/válvulas según el procedimiento de la labor.
5. Vaciar, limpiar o drenar tanque, bomba, mangueras y boquillas de acuerdo con el
   líquido utilizado, la TS-28 física y las obligaciones agrícolas aplicables. Una
   bomba que haya manejado productos agrícolas no se usa para agua de consumo.
6. Ejecutar el procedimiento aprobado de apagado/aislamiento de batería cuando esté
   disponible. Hasta que se documente, esta acción la realiza el personal autorizado.
7. Apagar la radio solo después de que Rafa esté en condición segura y desenergizado.
8. Registrar fugas, daños, consumo inusual, alarmas o incidentes en la bitácora.

## 9. Cuándo no operar y a quién llamar

No operar y notificar al responsable cuando exista cualquiera de estas condiciones:

- correa o poleas expuestas, guarda faltante o daño mecánico;
- olor a quemado, humo, chispa, caja húmeda, cable suelto o batería/inversor anormal;
- motor húmedo, ventilación obstruida, manómetro dañado o ilegible, pérdida de
  succión, presión/caudal inestable o parada inesperada del motor;
- fuga de líquido, boquilla o manguera desprendida;
- respuesta inesperada al joystick, giro incorrecto, movimiento sin orden o paro que
  no detiene la tracción;
- radio descargada, controles dañados o estado de CH5 desconocido;
- terreno, clima, pendiente, carga o visibilidad fuera de los límites aprobados;
- ausencia de EPP, procedimiento químico, SDS/FDS o responsable de la aplicación.

**Contacto técnico:** _Pendiente de asignar._

**Responsable operativo/agronómico:** _Pendiente de asignar._

## 10. Tarjeta rápida para el operador

~~~text
ANTES
  Área libre · Guarda de correa instalada · Sin fugas · Radio cargada
  Joystick centro · CH5 abajo (RC deshabilitada, no corte total) · CH6 extremo izquierdo · Bomba OFF

MOVER
  Encender radio y Rafa con el procedimiento aprobado
  Joystick centrado → CH5 arriba → esperar neutral
  Arriba/abajo: avance/reversa · Izquierda/derecha: giro
  Iniciar lento; no superar media escala con tanque lleno

PARAR
  Normal: joystick centro → CH5 abajo → verificar inmovilidad
  Riesgo de tracción: pulsador rojo/amarillo (paro de tracción, no paro total)
  Bomba: inversor OFF (el pulsador de tracción NO apaga la bomba)

PROHIBIDO
  Operar con correa expuesta, personas en zona de riesgo, fuga o caja abierta
~~~

## 11. Pendientes para convertir este borrador en manual final

1. Registrar el build/SHA/perfil **rafa** que será la versión de operación PPM.
2. Documentar una aceptación física de piso para avance, reversa, giros, parada,
   pérdida de radio, CH5 físico, pulsador de tracción y ausencia de reactivación por
   una fuente LAN.
3. Registrar una captura PPM sobre ese SHA con ocho canales estables y revisión de
   contadores de tramas inválidas, overflow y rechazo.
4. Verificar la escala física CH6: izquierda ≈ 50 %, centro ≈ 75 %, derecha ≈ 100 %
   del límite de perfil, tanto en avance como en giro; no tratar el mínimo como cero.
5. Instalar e inspeccionar una guarda fija íntegra de correa antes de liberar la
   motobomba o la aspersión.
6. Identificar, fotografiar y etiquetar el corte físico total de batería.
7. Confirmar ancho exterior, peso total, radio de giro, pendiente, terreno, velocidad
   máxima y distancias de exclusión operativas.
8. Fotografiar y cotejar la placa, marca y variante de la TS-28 realmente instalada.
   Confirmar la relación de poleas, RPM de bomba, manómetro, retorno/bypass, cebado,
   presión/caudal, mantenimiento, drenaje y restricciones de operación para los dos
   brazos y 36 boquillas.
9. Fotografiar y cotejar la placa WEG del motor. Un técnico/electricista debe validar
   tensión/frecuencia reales, puesta a tierra, protección de sobrecarga, capacidad de
   arranque del inversor, ventilación y resguardo frente al agua/aspersión.
10. Definir los líquidos autorizados, SDS/FDS, EPP, limpieza, derrames y responsable
    agrícola.
11. Añadir fotografías aprobadas y diagramas con **FRENTE**, controles de radio,
    pulsador de tracción, inversor, batería, válvulas y zonas prohibidas.

## Base de este borrador

La configuración mecánica, hidráulica y de operación descrita aquí proviene de la
entrevista y fotografías del propietario en septiembre de 2026. El mapeo PPM se basa
en el perfil de firmware **rafa** y debe volver a verificarse contra el firmware
realmente instalado antes de capacitar operadores. Los detalles de la TS-28, productos
aplicados y procedimiento de aislamiento eléctrico total aún requieren verificación
física en Rafa.

### Fuentes externas consultadas

- [TS-28, página de producto de Tanong/DIAMOND](https://www.tanong-co.com/tw/product-detail/htp-power-sprayer-ts-28/)
  y su manual de bomba estacionaria por correa con regulador manual, consultados el
  17 de septiembre de 2026. Sus datos son referencia para la variante indicada, no
  evidencia de que la unidad física de Rafa tenga la misma placa, RPM o configuración.
- [Manual TS-28 de Tanong/DIAMOND (PDF)](https://file.tanong-co.com//TW-manual/HTP%20Power%20Sprayer%20%28Stationary%20Belt-Drive%29/HTP%20Power%20Sprayer%20%28With%20Manual%20Pressure%20Regulator%29/SR-03-100-05%20TS-28%20Power%20Sprayer%20Manual-TW.pdf)
  es el original en chino consultado para esta síntesis; en caso de duda, prevalece
  la placa de Rafa y una revisión técnica antes de operar.
- [Referencia comercial del motor monofásico WEG 3 HP/IP21](https://fabriles.com.co/producto/motor-monofasico-weg-3-hp-1750-rpm-ip21-110-220v/),
  consultada el 17 de septiembre de 2026. La placa física y el manual WEG aplicable
  prevalecen sobre esta ficha comercial.
- [Familia WEG de motor monofásico IP21 de 3 cv](https://www.weg.net/catalog/weg/BR/en/Electric-Motors/Single-Phase/General-Purpose/Open-Steel-Motor-%28IP21%29/Motor-3-cv-4P-G56H-1Ph-110-127-220-254-V-60-Hz-IC01---ODP---Foot-mounted/p/14421152),
  usada solo como referencia de familia; requiere cotejo contra la placa física.
- [Guía WEG de especificación y grados IP](https://static.weg.net/medias/downloadcenter/ha0/h5f/WEG-motors-specification-of-electric-motors-50039409-brochure-english-web.pdf),
  usada para interpretar de forma conservadora el alcance de IP21. No sustituye el
  manual ni el diagrama de conexión del motor físico de Rafa.
