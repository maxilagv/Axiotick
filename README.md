# Axiotick

*Motor de decisión cuantitativo de baja latencia, construido para que cada señal, cada orden y cada riesgo asumido se puedan medir, explicar y reproducir.*

**Estado del proyecto: etapa inicial, en desarrollo activo.** El núcleo del motor de decisión — señales, régimen de mercado, gobernanza de estrategias, riesgo y backtesting — está implementado y testeado. La conectividad real con exchanges, el pipeline de IA en producción, la alta disponibilidad y los controles de seguridad de nivel institucional **todavía no existen**: son la siguiente frontera del roadmap, no una promesa ya cumplida.

> **Nota de idioma.** Este README resume el proyecto en español para que el equipo fundador pueda revisarlo con precisión. La documentación técnica exhaustiva — ADRs, arquitectura objetivo, checklist de live trading, benchmarks — vive en inglés dentro de `docs/`, y está enlazada a lo largo de todo este documento.

---

## Tabla de contenidos

- [Qué es Axiotick](#qué-es-axiotick)
- [La tesis del producto](#la-tesis-del-producto)
- [Los cuatro modos de operación](#los-cuatro-modos-de-operación)
- [Estado actual: qué es real y qué es objetivo](#estado-actual-qué-es-real-y-qué-es-objetivo)
- [Arquitectura general](#arquitectura-general)
- [El motor de decisión, en detalle](#el-motor-de-decisión-en-detalle)
- [Backtesting y validación estadística](#backtesting-y-validación-estadística)
- [Latencia: filosofía, reglas y números reales](#latencia-filosofía-reglas-y-números-reales)
- [Inteligencia artificial: hoy y el diseño objetivo](#inteligencia-artificial-hoy-y-el-diseño-objetivo)
- [Estrategia de lenguajes](#estrategia-de-lenguajes)
- [Estructura del repositorio](#estructura-del-repositorio)
- [Construcción, pruebas y benchmarks](#construcción-pruebas-y-benchmarks)
- [Hoja de ruta (roadmap)](#hoja-de-ruta-roadmap)
- [Calidad, CI y contribución](#calidad-ci-y-contribución)
- [Aviso legal y de riesgo](#aviso-legal-y-de-riesgo)

---

## Qué es Axiotick

Axiotick es una plataforma de trading cuantitativo de baja latencia, escrita en C++20, pensada para traders profesionales, fondos pequeños, equipos cuantitativos y constructores de tecnología financiera. No es un "bot de trading" genérico ni un producto terminado: es un **motor de decisión** — auditable, medible y diseñado para fallar siempre hacia el lado seguro (*fail-closed*) — sobre el que se construyen investigación, backtesting, paper trading y, eventualmente, ejecución en vivo controlada.

El problema que Axiotick busca resolver no es "encontrar una señal que gane dinero". Ese problema ya lo intentan resolver miles de bots y scripts, casi todos con el mismo defecto de fondo. El problema real — el que rompe a la mayoría de los sistemas de trading retail y de etapa temprana — es que investigación, ejecución, riesgo y operaciones se mezclan en un flujo opaco: no hay mediciones de latencia reproducibles, los costos de transacción (fees, spread, slippage, funding) se ignoran o se subestiman, no hay traza de auditoría, no hay *replay* determinista de lo que pasó, y no hay gobierno de riesgo que separe con claridad "estoy probando" de "estoy arriesgando capital real".

Axiotick trata la **latencia**, el **riesgo**, la **trazabilidad** y la **validación estadística** como requisitos de ingeniería de primera clase — no como una capa que se agrega después de que "la estrategia funciona".

---

## La tesis del producto

La mayoría de las estrategias de trading no fallan porque el modelo prediga mal. Fallan porque nadie filtra, en tiempo real, **cuándo** conviene confiar en una señal. Casi cualquier estrategia tiene una ventaja genuina (*edge*) en algún régimen de mercado — y la pierde, o directamente la invierte, en otro. Sin un mecanismo que lo detecte mientras ocurre, ese edge se diluye entre costos de transacción, ruido estadístico y condiciones de mercado adversas.

La tesis de producto de Axiotick es que un motor de decisión que combine:

1. **Filtrado por valor esperado neto** — el EV-Gate: solo se opera cuando la señal es rentable *después* de costos, no antes.
2. **Conciencia del régimen de mercado** — una señal no vale lo mismo en tendencia, en rango o en estrés; el motor lo sabe y ajusta el umbral de aceptación en consecuencia.
3. **Gobernanza estadística del ciclo de vida de cada estrategia** — detectar la degradación del edge con tests estadísticos y actuar (observar, reducir tamaño, poner en cuarentena, deshabilitar) antes de que duela, no después.

puede aumentar de forma sustancial la probabilidad de que las decisiones que efectivamente se ejecutan terminen en un retorno de inversión alto para el operador experto — en lugar de dejar esa probabilidad librada al tamaño de muestra y a la suerte del régimen de mercado vigente en el momento de operar.

> **Objetivo interno de investigación: +60% de probabilidad de ROI alto para el operador experto, respecto de operar la misma estrategia sin este stack de filtrado.**
>
> Este número es una **hipótesis de producto que guía las prioridades de ingeniería**, no un resultado ya demostrado ni una garantía de rentabilidad. Se valida con evidencia estadística *out-of-sample* — backtesting contrafactual (Modo B), validación *walk-forward*, remuestreo de Monte Carlo y, más adelante, paper trading — antes de que cualquier capital real dependa de él. Ningún número de este documento debe leerse como promesa de rentabilidad. Ver [Aviso legal y de riesgo](#aviso-legal-y-de-riesgo).

Como primera evidencia de mecanismo — mínima, de una sola estrategia de demostración, **no** una prueba de la meta completa del 60% — el backtest contrafactual de referencia (documentado en [`docs/ADR/0013-strategy-lifecycle-governance.md`](docs/ADR/0013-strategy-lifecycle-governance.md)) muestra una estrategia SMA-crossover cuyo desempeño se invierte según el régimen de mercado:

| Régimen | EV neto previsto | Retorno realizado |
|---|---|---|
| Tendencia (*Trend*) | +24 bps | **+67 bps** (el edge fue mejor de lo previsto) |
| Estrés (*Stress*) | +52 bps | **-80 bps** (el edge se invirtió por completo) |

Es exactamente el tipo de inversión de señal que el clasificador de régimen y la gobernanza de ciclo de vida están diseñados para detectar y bloquear antes de que llegue a ejecución. Es el mecanismo detrás de la tesis — todavía no la evidencia estadística de la meta completa.

---

## Los cuatro modos de operación

Axiotick separa explícitamente cuatro modos de uso, y el motor de decisión es **el mismo código** en los cuatro — no una reimplementación paralela para cada uno. Esto es intencional: el backtesting en Modo B ejecuta literalmente el mismo camino de código (`SignalEngine → EVGate → OrderManager → RiskManager → Order Book`) que correría en producción, para que el comportamiento validado en backtest no pueda divergir silenciosamente del comportamiento en vivo.

| Modo | Qué hace | Estado |
|---|---|---|
| **Research** | Análisis en Python, ingeniería de features, entrenamiento de modelos, NLP, estudios históricos | Conceptual / placeholder |
| **Backtesting** | Simulación determinista con costos, spread, slippage, funding, drawdown y validación out-of-sample | Implementado y testeado |
| **Paper trading** | Ejecución de estrategias en tiempo real contra venues simulados, con perfiles de latencia y controles de riesgo reales | No iniciado |
| **Live trading** | Ejecución controlada en producción, solo tras validación, monitoreo, kill switches y aprobación operativa | Bloqueado hasta cumplir el [checklist de preparación](docs/LIVE_TRADING_READINESS_CHECKLIST.md) |

---

## Estado actual: qué es real y qué es objetivo

Axiotick documenta explícitamente sus propias afirmaciones como **implementado**, **parcial** u **objetivo** — nunca como terminado si no lo está. Esta es la sección más honesta del proyecto, y se actualiza con cada bloque de trabajo entregado. Snapshot: julio de 2026, tras los Bloques 1-3 del motor de decisión.

**Madurez estimada:** la auditoría formal ([`docs/CURRENT_STATE_AUDIT.md`](docs/CURRENT_STATE_AUDIT.md)) midió una base de **4/10 - 4.5/10** contra el objetivo a un año, antes de que se entregara el motor de decisión. Con las fases de Signal Engine, Risk Engine (parcial) y Backtesting ya implementadas y testeadas, la madurez global actual se estima en **5/10 - 5.5/10**: el motor de decisión está notablemente adelantado respecto de esa línea; la conectividad de mercado, la IA productiva y las operaciones de nivel institucional siguen detrás.

Evidencia concreta: 38 objetivos de CTest, dos binarios de demostración end-to-end (`argentum_signal_demo`, `argentum_backtest_demo`), y los ADRs [0011](docs/ADR/0011-ev-gated-signal-engine.md)-[0013](docs/ADR/0013-strategy-lifecycle-governance.md).

### Implementado y testeado

- Backend en C/C++20 con CMake, perfil de compilación de baja latencia (ver [Latencia](#latencia-filosofía-reglas-y-números-reales)).
- Bus de eventos en proceso con colas acotadas y tres políticas de backpressure configurables (`DropNewest`, `DropOldest`, `Block` con timeout opcional), con métricas por tópico (profundidad de cola, drops, publish latency).
- Protocolo de mensajes binario versionado (encabezado V1/V2, CRC32 opcional en V2) y codec de ticks de mercado; esquema FlatBuffers (`backend/schema/argentum.fbs`) para `MarketTick`, `Order` y `Trade`.
- Order book con matching por prioridad precio-tiempo, fills parciales con reposo de residual, cancelación y modificación, asignador de memoria en pool (131.072 nodos de capacidad) y cálculo de VWAP para modelar slippage.
- OMS con máquina de estados de órdenes, semántica `GTC` / `IOC` / `FOK`, serialización de todas las mutaciones del order book bajo un único dominio de lock, e integración con el *event journal* (las órdenes quedan journaladas con el id de la señal que las originó, para correlación en el replay).
- **EV-Gate**: filtro de valor esperado neto sobre un modelo de costos completo (fees, spread, slippage, impacto de mercado, funding × tiempo de holding esperado), *fail-closed* ante entradas inválidas, con traza de auditoría explicable por cada decisión. Latencia medida: p50 ~100 ns, p99 ~300 ns por evaluación.
- **Clasificador de régimen de mercado**: agregación de ticks a barras y clasificador determinista por reglas (`Trend` / `Range` / `Stress` / `VolExpansion` / `Unknown`) que condiciona los umbrales del EV-Gate y la elegibilidad de cada estrategia. Latencia medida: p50 ~600 ns, p99 ~900 ns por barra.
- **Gobernanza de ciclo de vida de estrategias**: transiciones automáticas Activa / Bajo observación / Cuarentena / Deshabilitada, impulsadas por detección de quiebre de Page-Hinkley, piso de Sharpe móvil y t-stat de decaimiento de EV, con todas las transiciones auditadas y override manual del operador.
- Motor de riesgo: límites de valor de orden, exposición bruta y por símbolo; ledger de reservas de riesgo por `order_id`; contabilidad de posición a costo promedio con PnL realizado y no realizado; límite de pérdida diaria con kill switch automático (solo reseteable manualmente).
- Backtesting: corridas contrafactuales por el camino exacto de decisión de producción, métricas Sharpe/Sortino/máximo drawdown/VaR-CVaR histórico, bootstrap de Monte Carlo determinista, validación walk-forward out-of-sample con fábrica de estrategias, reporte por régimen (realizado vs. previsto) y barridos de sensibilidad a costos/estrés.
- Ruteo inteligente de órdenes (v1 top-of-book, v2 con profundidad L2 y re-ruteo tras fill parcial) y simulador de ejecución con métricas de calidad por venue.
- Gateway HTTP/WebSocket con control de tokens y rate-limit.
- Escritura asíncrona de ticks de mercado con fallback a CSV (rotación, headers, fsync opcional) y ruta opcional a TimescaleDB.
- Event journal en JSONL append-only con secuencia monótona y tests orientados a replay determinista.
- Benchmarks de matching, pipeline y régimen, con una línea base registrada en [`docs/benchmarks/`](docs/benchmarks/2026-07-decision-engine-baseline.md).
- Estructura de repositorio *backend-only*, con apps, módulos, benchmarks y tests separados.

### Todavía en desarrollo (o directamente un stub)

- Conectividad real multi-exchange/broker y datos de mercado en vivo (funding, open interest, feeds de liquidaciones). Hoy la ingesta es orientada a archivo/replay, no a venues en vivo.
- **Pipeline de IA/ML en producción**: el bridge actual (`backend/include/ml/python_bridge.hpp`) es, literalmente, un mock — su método `connect()` es un comentario (`// In prod: zmq_connect(socket, endpoint)`) y su `predict()` devuelve `0.5` fijo, sin importar la entrada. No existe todavía feature store ni model registry. Ver [Inteligencia artificial](#inteligencia-artificial-hoy-y-el-diseño-objetivo) para el diseño objetivo y por qué esto ya está contemplado de forma segura en el EV-Gate.
- Paper trading contra datos en vivo; adaptadores de venues de ejecución reales más allá de la simulación.
- Controles de margen/correlación a nivel de portafolio y gating de VaR en tiempo real.
- Capa de servicio en Java (consultas de reporting/auditoría, snapshots de riesgo, model registry).
- Alta disponibilidad, despliegue distribuido y operación sostenida al 99.9% de uptime.
- Seguridad, cumplimiento, vigilancia y gobernanza de nivel empresarial (RBAC, MFA, ciclo de vida de secretos, rotación de claves).
- Evidencia reproducible de escala para 100k-1M eventos/seg y 10k-50k instrumentos — hoy son objetivos de producto, no mediciones logradas.

---

## Arquitectura general

```text
                       ┌─────────────────────────────────────────┐
                       │  INVESTIGACIÓN / IA (fuera del hot path) │
                       │  Python · feature store · model registry │
                       │  · notebooks reproducibles                │
                       └────────────────────┬──────────────────────┘
                                            │ modelos versionados
                                            ▼
 Mercado → Gateway de Datos → Normalizador → Bus de Eventos (colas acotadas, backpressure)
                                                      │
                    ┌─────────────────────────────────┼─────────────────────────────────┐
                    ▼                                 ▼                                 ▼
             Order Book Engine                Clasificador de Régimen          Contexto histórico /
          (matching, VWAP, estado)         (Trend/Range/Stress/VolExp)            feature store
                    │                                 │                                 │
                    └────────────────┬────────────────┴────────────────┬────────────────┘
                                     ▼                                 ▼
                              Signal Engine  ─────────────────▶     EV-Gate
                                     │             (valor esperado neto vs. umbral por régimen)
                                     ▼
                    Gobernanza de ciclo de vida de estrategias
                 (Activa / Bajo observación / Cuarentena / Deshabilitada)
                                     │   (si el gate y el ciclo de vida lo permiten)
                                     ▼
                               Motor de Riesgo
                (límites, ledger de reservas por orden, kill switch)
                                     │
                                     ▼
                                    OMS
                      (GTC / IOC / FOK, máquina de estados)
                                     │
                                     ▼
                    Execution / Smart Routing Gateway
                    (paper o vivo, ruteo L2, KPIs de ejecución)
                                     │
                                     ▼
                               Venue / Broker

 Todo evento de este flujo también alimenta:
   Persistencia (journal JSONL, CSV/TimescaleDB) → Replay determinista → Audit log (JSON) → Observabilidad
```

Componentes principales:

- **Market Data Gateway**: recibe datos de mercado externos de exchanges, brokers y proveedores de datos.
- **Normalizador**: convierte mensajes específicos de cada venue en eventos internos canónicos.
- **Bus de eventos**: mueve streams de eventos acotados y medibles a través del runtime.
- **Order Book Engine**: mantiene el estado local del libro y las primitivas de matching/simulación.
- **Clasificador de régimen**: etiqueta el estado del mercado (tendencia, rango, estrés, expansión de volatilidad) a partir de barras agregadas de ticks.
- **Signal Engine**: orquesta la señal candidata contra el estado de ciclo de vida y el EV-Gate.
- **EV-Gate**: calcula el valor esperado neto de cada señal y decide si es operable.
- **Gobernanza de ciclo de vida**: decide, con tests estadísticos, si una estrategia sigue activa, se reduce, se pausa o se deshabilita.
- **Motor de riesgo**: aplica controles de riesgo pre-trade y en tiempo de ejecución.
- **OMS**: posee el ciclo de vida de la orden, sus transiciones de estado y sus eventos de auditoría.
- **Execution Gateway**: rutea órdenes validadas hacia venues simulados o reales.
- **Backtesting / Paper Trading**: valida el comportamiento de la estrategia antes de arriesgar capital real.
- **Capa de IA / Research**: usa Python para investigación, entrenamiento y explicabilidad — nunca en el camino caliente de decisión.
- **Observabilidad y auditoría**: captura latencia, drops, decisiones, eventos y acciones del operador.

Ver [`docs/ARCHITECTURE_TARGET.md`](docs/ARCHITECTURE_TARGET.md) para la arquitectura objetivo completa (33 componentes) y [`docs/LANGUAGE_STRATEGY.md`](docs/LANGUAGE_STRATEGY.md) para la estrategia de lenguajes.

---

## El motor de decisión, en detalle

Esta es la parte del sistema donde vive la tesis del producto. Todo lo que sigue está implementado y testeado — no es diseño aspiracional.

### Order Book Engine

Libro de órdenes de dos lados (bids/asks) con niveles de precio como listas doblemente enlazadas (`PriceLevel`), un localizador de órdenes por id (`OrderLocator`) y un asignador de memoria en pool para reciclar nodos de orden (131.072 nodos de capacidad, 4096 niveles de precio por lado, configurables). El matching resuelve por prioridad precio-tiempo, soporta fills parciales con reposo del residual, cancelación, modificación y cancelación parcial, y calcula VWAP para modelar slippage. Es el componente sobre el que se apoyan tanto la ejecución simulada como el backtesting contrafactual.

### Clasificador de régimen de mercado

Dos piezas separadas y puras (sin efectos de lado, sin I/O):

- **`BarAggregator`**: agrega ticks a barras con semántica fija — barras alineadas por borde, un tick que cruza el borde abre la siguiente barra, no se fabrican barras vacías sintéticas ante gaps, y los ticks obsoletos se ignoran. La misma instancia sirve tanto al loop de backtest como al suscriptor en vivo del bus: mismos números, sin deriva entre backtest y producción.
- **`RegimeClassifier`**: clasificador determinista por reglas sobre features rolling — volatilidad realizada (retornos logarítmicos, ventana de 20 barras por defecto), ADX/ATR de Wilder (período 14, listo a partir de ~28 barras), percentil de ATR contra su propia historia (ventana de 100 barras) y z-score de volumen (ventana de 20 barras, excluyendo la barra actual).

Prioridad de reglas, de mayor a menor (la primera que matchea gana — seguridad antes que precisión):

1. **`Stress`**: z-score de volumen ≥ 2.5 y volatilidad realizada en su propio percentil alto.
2. **`VolExpansion`**: z-score de volumen ≥ 1.5.
3. **`Trend`**: ADX ≥ 25 (configurable).
4. **`Range`**: ADX < 25 y percentil de ATR < 0.40.
5. **`Unknown`**: warmup incompleto (mínimo 20 barras) o zona ambigua.

Un crash direccional violento satisface simultáneamente las condiciones de `Stress` y `Trend` — y por diseño debe clasificarse como `Stress`. La confianza de cada clasificación es un margen saturante (`excess / (1 + excess)`, siempre menor a 1) sobre la métrica decisiva de la regla. Declarar `Unknown` en zonas ambiguas es deliberado: es el propio EV-Gate el que decide, por estrategia, si `Unknown` es operable o no. El calibrado estadístico de los umbrales (HMM/GMM sobre datos históricos, en Python, exportado como JSON versionado) está planificado para un bloque futuro; la estructura de reglas no cambia cuando eso ocurra.

### EV-Gate: el filtro de valor esperado neto

El corazón de la tesis del producto. Es una librería *stateless*, sin I/O (`argentum_ev`), que calcula:

```text
ev_gross_bps = p_win * avg_win_bps + (1 - p_win) * avg_loss_bps

cost_bps     = 2 * taker_fee_bps
             + 2 * half_spread_bps
             + slippage_bps
             + market_impact_bps
             + funding_bps_per_hour * expected_holding_hours

ev_net_bps   = ev_gross_bps - cost_bps
```

Una señal se acepta solo si se cumplen **todas** estas condiciones:

- `ev_net_bps >= min_ev_bps_threshold * regime_multiplier` — el umbral mínimo está deliberadamente por encima de cero: un EV apenas positivo, dentro del ruido de estimación, no es operable.
- `confidence >= min_confidence` (la confianza que entrega el clasificador de régimen).
- El régimen actual está permitido para esa estrategia (`allowed_regimes_mask`).
- El estado de ciclo de vida de la estrategia no bloquea la operación (ver más abajo).

**Diseño *fail-closed*:** entradas inválidas — probabilidades fuera de `[0, 1]`, NaN, notional no positivo, costos negativos, un enum de régimen desconocido — rechazan inmediatamente con `invalid_inputs`. Si el bridge de modelos de ML no responde a tiempo, el gate rechaza con `ml_bridge_unavailable` — **nunca** sustituye la predicción faltante por un valor por defecto. Este contrato de seguridad ya está construido en el gate desde antes de que exista un bridge de ML real, precisamente para que la futura integración de modelos no pueda degradar silenciosamente la disciplina de riesgo.

Cada decisión — aceptada o rechazada — emite un evento `signal_decision` estructurado en el audit log: id de estrategia, versión de modelo, referencia al snapshot de features, régimen y confianza, `p_win`/`avg_win`/`avg_loss`, cada componente de costo, EV bruto/costo/neto, el umbral usado, el motivo de aceptación o rechazo, el id de la orden resultante (si aplica) y una razón en lenguaje natural.

Latencia medida (Release, MSVC, ver [Latencia](#latencia-filosofía-reglas-y-números-reales)): p50 ~100 ns, p95 ~100 ns, p99 ~300 ns, máximo ~600 ns por evaluación — cinco órdenes de magnitud por debajo del presupuesto de decisión de 25 ms. La matemática del gate no es, ni de lejos, el cuello de botella del sistema.

### Gobernanza de ciclo de vida de estrategias

Ninguna estrategia opera para siempre sin supervisión. Cada estrategia tiene un estado — `Active`, `UnderObservation`, `Quarantined`, `Disabled` — y ese estado se decide con tests estadísticos, no con intuición:

```text
Activa          -> Bajo observación :  alarma de Page-Hinkley O ruptura del piso de Sharpe móvil
Bajo observación -> Activa          :  20 evaluaciones consecutivas "limpias"
Bajo observación -> Cuarentena      :  ruptura de drawdown de la estrategia O alarma de decaimiento de EV
Activa          -> Cuarentena       :  ruptura de drawdown (protección de capital inmediata)
Cuarentena      -> Activa           :  únicamente por decisión manual del operador (force_state)
Cuarentena      -> Deshabilitada    :  automático, a la 3ª cuarentena dentro de una ventana de 90 días
```

Mientras una estrategia está `UnderObservation`, sigue operando pero con un recorte de tamaño configurable (25% por defecto) — degradación gradual en vez de apagado abrupto. `Quarantined` y `Disabled` bloquean directamente en el EV-Gate.

**Atribución por horizonte, no por posición.** La forma obvia de medir el resultado de una señal — llevar una posición contable por estrategia — se rompe en cuanto una señal cierra una posición que abrió otra (exactamente lo que hace un cruce de medias móviles: la cruz bajista cierra el long y abre el short en el mismo fill). En cambio, cada fill aceptado se evalúa contra el horizonte que la propia señal declaró (`expected_holding_hours`): al cumplirse ese horizonte, se compara el retorno realizado contra lo que la señal prometió, de forma independiente de cualquier otra señal concurrente.

**Detector de quiebre de Page-Hinkley** (forma estándar de detección de caída, sobre `realized_bps`):

```text
mean_t = media móvil (Welford)
m_t    = m_(t-1) + (x_t - mean_t + delta)      delta = 0.5 (por defecto)
M_t    = max(M_(t-1), m_t)
PH_t   = M_t - m_t   ->  alarma si PH_t > lambda   lambda = 5.0 bps (por defecto,
                                                    recalibrable a ~2-3x el desvío
                                                    estándar propio de la estrategia)
```

Una racha ganadora empuja `m` hacia arriba mientras `M` lo sigue de cerca (`PH ≈ 0`); una caída sostenida del retorno hunde `m` mientras `M` recuerda el pico anterior, y `PH` crece hasta disparar la alarma.

Además del quiebre de Page-Hinkley, la gobernanza evalúa:

- **Piso de Sharpe móvil**: ratio de Sharpe crudo sobre una ventana de 30 muestras, comparado contra el Sharpe de referencia registrado en el backtest de la estrategia; alarma si cae por debajo del 50% de esa referencia. Sin una referencia registrada, este chequeo queda deshabilitado por diseño — no hay nada contra qué degradar.
- **T-stat de decaimiento de EV**: t-stat unilateral de `(realizado - previsto)` sobre un mínimo de 50 evaluaciones; alarma si `t < -2.0`.
- **Drawdown de la estrategia**: pico-a-valle sobre el PnL acumulado en notional, con umbral configurable.

Toda transición — automática o manual — queda auditada como evento estructurado `strategy_transition`, con motivo y timestamp. Ningún detector se "olvida" tras una transición: las ventanas rolling no se resetean, así que la recuperación se gana sanando la ventana, no por amnesia del sistema.

### Motor de riesgo

Controles pre-trade y en tiempo de ejecución:

- Límite de valor de orden, exposición bruta por símbolo y por portafolio, y límites de posición neta configurables por símbolo.
- **Ledger de reservas por `order_id`**: al validar una orden se reserva exposición al precio y lotes de esa orden específica; al ejecutarse (`on_fill`) se libera la reserva al precio reservado y se contabiliza la exposición real al precio ejecutado; al cancelarse (`on_cancel`) se libera la reserva restante. Esta separación entre precio reservado y precio de ejecución elimina un problema real de diseño previo: si la reserva y la liberación se calculaban ambas sobre el precio de ejecución, un precio de fill distinto al de la reserva original hacía que la exposición comprometida "derivara" con el tiempo.
- **Kill switch de pérdida diaria**: contabilidad de posición a costo promedio por símbolo, PnL realizado y no realizado (marcado a mercado), baseline de reinicio a la medianoche UTC. El switch es atómico, bloquea `check_order()` mientras sigue contabilizando fills en vuelo, y queda auditado tanto al dispararse como al resetearse. **Nunca se resetea automáticamente por el paso del día** — reactivarlo es siempre una decisión manual del operador.
- **VaR paramétrico** (media-varianza, z-scores 1.65 para 95% y 2.33 para 99%) y **VaR/CVaR histórico** (percentil empírico sobre retornos ordenados, sin asumir normalidad, convención de pérdida positiva). El histórico es el preferido para evaluar estrategias reales, porque los retornos de trading suelen tener colas más pesadas de lo que el supuesto de normalidad del método paramétrico tolera.

### OMS y semántica de órdenes

Máquina de estados de orden (`accepted`, `rejected`, `resting`, `partially filled`, `filled`, `canceled`, `replaced`) con semántica de *time-in-force* explícita:

- **`GTC`**: el residual de una orden límite queda reposando en el libro.
- **`IOC`**: se ejecuta lo posible de inmediato y se cancela el residual.
- **`FOK`**: se rechaza la orden completa si no hay liquidez inmediata suficiente para llenarla entera; si la hay, se ejecuta entera. El OMS pre-chequea la liquidez ejecutable antes de reservar riesgo, para no reservar exposición por una orden que de todos modos será rechazada.

Todas las mutaciones del order book (submit, cancel, modify) se serializan bajo un único dominio de lock (el mutex del OMS) — una simplificación deliberada de esta primera fase, sin sharding por instrumento todavía, que prioriza la ausencia de condiciones de carrera por sobre el throughput máximo. El sharding por instrumento es trabajo planificado, no una limitación permanente.

### Ruteo inteligente y calidad de ejecución

Capa de ruteo multi-venue en dos generaciones:

- **v1** (top-of-book): abstracciones normalizadas de venue (`VenueQuote`, `VenueDescriptor`), un adaptador FIX para normalización de datos de mercado, un router que puntúa por costo efectivo (fee + latencia) y un simulador de ejecución con modelado de posición en cola, probabilidad de fill, slippage y perfil de latencia.
- **v2** (profundidad L2): el modelo de venue se extiende a niveles de profundidad (`QuoteLevel`, `VenueOrderBookSnapshot`), el router gana re-ruteo del residual tras un fill parcial, el simulador pasa a multi-pasada consumiendo profundidad en cada fill, y un `ExecutionQualityTracker` mide fill ratio, slippage y latencia (p50/p95) por venue.

### Bus de eventos, protocolo de mensajes y persistencia

- **Bus**: colas acotadas por tópico con tres políticas de backpressure configurables (`DropNewest`, `DropOldest`, `Block` con timeout opcional) y métricas de profundidad de cola, drops y latencia de publicación por tópico.
- **Protocolo**: encabezado versionado — V1 (legado) y V2 (con flags y CRC32 opcional) — decodificado de forma retrocompatible; payloads definidos en un esquema FlatBuffers (`backend/schema/argentum.fbs`) para `MarketTick`, `Order` y `Trade`, habilitable vía la opción de build `ARGENTUM_USE_FLATBUFFERS`.
- **Persistencia**: cola MPSC acotada que desacopla la escritura de disco/base de datos del camino caliente; conexión persistente a TimescaleDB cuando está habilitada, con fallback a CSV con rotación y fsync opcional.
- **Event journal**: append-only en JSONL, con secuencia monótona y validación de timestamp en el replay; seis tipos de evento (`order_accepted`, `order_rejected`, `trade_executed`, `order_canceled`, `order_replaced`, `gateway_rejected`) a partir de los cuales se puede reconstruir el estado completo (órdenes activas, historial, exposición comprometida/llenada, posición neta).

---

## Backtesting y validación estadística

El backtesting de Axiotick tiene dos modos, y solo uno de ellos alimenta la tesis del producto de forma creíble:

- **Modo A**: análisis post-mortem sobre fills ya ejecutados (cargados de CSV o journal).
- **Modo B — contrafactual**: siembra una orden sintética de *maker* en un `OrderBook` real al precio de referencia del tick, y somete la señal candidata al camino de decisión **exacto** de producción (`SignalEngine → EVGate → OrderManager → RiskManager → matching`), cancelando el residual del maker al final. Esto significa que el backtest **ejecuta literalmente el mismo código** que correría en vivo — el argumento anti-deriva más fuerte que puede ofrecer un backtester, porque no hay una segunda implementación paralela que pueda desalinearse silenciosamente de la primera.

Sobre esa base, el motor calcula:

- Sharpe, **Sortino** (objetivo MAR=0, semidesviación anualizada con raíz de 252), máximo drawdown, Calmar, profit factor y win rate.
- **VaR y CVaR históricos al 95%**, empíricos y sin asumir normalidad — elegidos sobre el VaR paramétrico preexistente porque los retornos de las estrategias evaluadas violan el supuesto de colas normales.
- **Bootstrap de Monte Carlo**: remuestreo con reposición de los fills, i.i.d., con semilla fija (`mt19937_64`, bit-determinista), reportando P5/P50/P95 por métrica. Está documentado explícitamente como una cota de robustez sobre la composición/orden de la muestra — **no** una simulación de escenarios de mercado, porque el remuestreo destruye la autocorrelación temporal.
- **Validación walk-forward**: requiere una fábrica de estrategias (`StrategyFactory`) que instancia una copia nueva por cada fold. Esto no es un detalle menor: reutilizar una sola instancia entre folds contamina silenciosamente la independencia out-of-sample (medias móviles, contadores de confirmación y demás estado interno se filtran de un fold al siguiente). Los datos de entrenamiento se registran pero permanecen inertes — las estrategias basadas en reglas no reentrenan; esto es evaluación out-of-sample, no optimización de hiperparámetros, y no debe presentarse como tal.
- **Barrido de sensibilidad a costos**: un decorador (`CostScaledStrategy`) escala únicamente los componentes de costo en bps (nunca las probabilidades ni el horizonte), con multiplicadores por defecto de 0.5x/1.0x/1.5x/2.0x/3.0x/5.0x. La gobernanza de ciclo de vida se desarma deliberadamente durante el barrido — de lo contrario, el barrido mediría la interacción con la gobernanza en vez de medir la sensibilidad real a los costos.
- **Reporte por régimen**: construido a partir del stream de evaluaciones por horizonte (no filtrando fills por régimen, lo cual rompería la contabilidad cuando una posición se abre en un régimen y se cierra en otro). Cada muestra es autocontenida y lleva el régimen vigente en el momento de la señal — así se obtuvo la evidencia de la tabla en [La tesis del producto](#la-tesis-del-producto).

Los fills se reconstruyen leyendo el propio *journal* de la corrida desde cero, no pasándolos en memoria — una elección deliberadamente más lenta, porque garantiza que cada corrida de Modo B ejercite también el camino de auditoría y replay de punta a punta.

---

## Latencia: filosofía, reglas y números reales

**La latencia es una métrica de producto, no un detalle de implementación que se optimiza después.** Toda medición debe reportarse en percentiles — p50, p95, p99 y p99.9 cuando el tamaño de muestra lo permite — nunca solo como promedio; el promedio se acepta únicamente como contexto de apoyo.

### Reglas del camino caliente (hot path)

Cinco restricciones que se aplican a todo lo que esté entre la llegada de un dato de mercado y la decisión de riesgo:

1. Sin I/O de disco bloqueante en el camino de mercado → decisión → riesgo.
2. Sin ejecución de Python en caminos calientes en vivo.
3. Sin colas no acotadas en rutas de runtime sensibles a latencia.
4. Sin sleeps ocultos ni loops de reintento en la lógica de camino crítico.
5. Los logs deben ser acotados, asíncronos, o directamente estar fuera del camino caliente.

### Objetivos de latencia y escala (metas de producto, no garantías actuales)

| Métrica | Objetivo |
|---|---|
| Latencia de análisis de señal | p50 < 10 ms / p99 < 50 ms |
| Dato de mercado → decisión → chequeo de riesgo | p50 < 25 ms / p99 < 100 ms |
| Latencia de ejecución vía API pública | 50 ms a 300 ms, según venue y red |
| Ejecución HFT colocada | Fuera de alcance para un operador independiente normal |
| Eventos de mercado procesados | 100.000 a 1.000.000 por segundo |
| Instrumentos monitoreados | 10.000 a 50.000 activos/pares |
| Exchanges/brokers conectados | 8 a 15 |
| Datos históricos procesados | 10 TB a 100 TB |
| Ingesta diaria de datos | 100 GB a 2 TB/día |
| Recálculo de modelo | cada 1s, 5s o 1m, según la estrategia |
| Objetivo de uptime | 99.9% |

Ver el detalle completo en [`docs/LATENCY_AND_SCALE_TARGETS.md`](docs/LATENCY_AND_SCALE_TARGETS.md).

### Números reales medidos hoy (no proyecciones)

Línea base registrada en julio de 2026 ([`docs/benchmarks/2026-07-decision-engine-baseline.md`](docs/benchmarks/2026-07-decision-engine-baseline.md)), con metadata completa de entorno como exige la disciplina de benchmarking del proyecto:

| Item | Valor |
|---|---|
| CPU | AMD Ryzen 5 5500 (6 núcleos / 12 hilos) |
| Memoria | 32 GB |
| Sistema operativo | Windows 11 Pro |
| Compilador | MSVC (generador Visual Studio 18 2026), `/O2 /Oi /Ot /Oy /GL`, `/std:c++20` |
| Build | Release |

| Componente | p50 | p95 | p99 | Notas |
|---|---|---|---|---|
| `EVGate::evaluate()` | ~100 ns | ~100 ns | ~300 ns | máx ~600 ns; 5 órdenes de magnitud bajo el presupuesto de 25 ms |
| `RegimeClassifier::on_bar()` | 600 ns | 800 ns | 900 ns | p99.9 ~60 µs (ruido de scheduler); las barras llegan cada segundos-minutos |

La lectura honesta de estos números: la matemática de decisión (gate + régimen) **no** es el cuello de botella del sistema. El desafío real de latencia va a estar en los caminos de datos y features — parsing, deserialización, acceso a contexto histórico — no en el EV-Gate ni en el clasificador. Cualquier benchmark reportado debe declarar hardware, compilador, flags de build, tamaño de dataset, throughput, drops y profundidad de cola, sin excepción.

---

## Inteligencia artificial: hoy y el diseño objetivo

### Estado hoy: un stub, sin adornos

`backend/include/ml/python_bridge.hpp` es, con toda honestidad, un placeholder. La clase `PythonBridge` acepta un endpoint de ZeroMQ en su constructor; su método `connect()` no hace nada más que un comentario (`// In prod: zmq_connect(socket, endpoint)`); su método `predict()` devuelve `0.5` fijo, sin importar el vector de features recibido. No hay feature store, no hay model registry, no hay proceso de validación, promoción o rollback de modelos. Esta sección del roadmap ([Fase 09 - AI Research](docs/roadmap/fase-09-ai-research/README.md)) está, en sus propias palabras, en nivel "conceptual/placeholder".

Esto no es un defecto oculto: es exactamente lo que dice ser. El motivo por el que puede decirse con esta franqueza es que **el resto del motor de decisión ya está preparado para que esto sea seguro cuando deje de ser un stub**: el EV-Gate ya tiene un motivo de rechazo dedicado (`ml_bridge_unavailable`) que dispara cuando el bridge de modelos no responde a tiempo — y ese camino de fallo seguro se construyó **antes** de que existiera el bridge real, no después.

### Diseño objetivo

Cuando el pipeline de IA deje de ser un stub, debe cumplir estas reglas, sin excepción:

- **Python nunca bloquea el camino caliente.** Vive en investigación, entrenamiento, NLP y análisis offline — nunca entre la llegada de un dato de mercado y la decisión de riesgo. Esta es una regla vinculante de [`docs/LANGUAGE_STRATEGY.md`](docs/LANGUAGE_STRATEGY.md), no una sugerencia.
- **Ningún modelo se promueve a runtime sin evidencia de validación out-of-sample.** Igual que una estrategia basada en reglas necesita walk-forward, un modelo necesita su propio gate de promoción.
- **Toda estrategia en runtime referencia una versión explícita de modelo.** Reproducibilidad total: se puede saber, para cualquier decisión pasada, exactamente qué versión de qué modelo la produjo.
- **Explicabilidad obligatoria.** Toda señal asistida por un modelo debe producir el mismo tipo de payload explicativo que ya produce el EV-Gate para señales basadas en reglas — no una caja negra con un número de confianza sin contexto.
- **Detección de drift y cuarentena de modelos**, con la misma filosofía que ya rige la gobernanza de ciclo de vida de estrategias: un modelo que se degrada se pone en observación o se retira, automáticamente y con auditoría, no por decisión manual tardía.
- **Feature store en dos capas**: offline (generación batch, medida por throughput) y online (consulta de baja latencia, medida y explícitamente fuera del camino caliente).
- **Dataset y features versionados**, con notebooks reproducibles que registran qué dataset, qué features y qué métricas produjeron cada modelo.

Requisitos de testing ya definidos para cuando esta fase se implemente: tests de cálculo de features, tests de validación de metadata de modelo, tests del gate de promoción, y tests de reproducibilidad sobre corridas de investigación de muestra.

---

## Estrategia de lenguajes

- **C++20** es el lenguaje del camino caliente: bus de eventos, codecs, order book, OMS, chequeos de riesgo, adaptadores de ejecución y los caminos de replay/benchmark donde la latencia de runtime importa. Perfil de compilación agresivo: en MSVC, `/W4 /permissive- /O2 /Oi /Ot /Oy /GL` con `/LTCG` en el link; en GCC/Clang, `-Wall -Wextra -Wpedantic -Werror -O3 -march=native -mtune=native -funroll-loops`, con excepciones y RTTI deshabilitados en el perfil estricto.
- **Python** está confinado a investigación: análisis cuantitativo, exploración de features, entrenamiento y evaluación de modelos, NLP sobre noticias/filings/eventos macro, notebooks con entradas controladas y salidas reproducibles. Nunca debe bloquear el camino de mercado → decisión → riesgo; sus modelos deben versionarse antes de usarse en runtime.
- **Rust** está deliberadamente diferido. No se adopta por moda, ni para reescribir módulos de C++ ya estables, ni para componentes pequeños que solo agregarían complejidad de build. Es una opción futura válida únicamente cuando un componente específico tenga una razón técnica clara y documentada — por ejemplo, conectores externos con concurrencia compleja, o parsers de alta velocidad donde la seguridad de memoria valga el costo de integración — con un diseño escrito que muestre el beneficio de seguridad, el impacto en latencia, el impacto en el build y un plan de tests.
- **Java** aparece como capa de servicio futura y planificada (no implementada aún) para reporting/auditoría, snapshots de riesgo y model registry — fuera del camino caliente de decisión.

Ver el detalle completo, con los riesgos de cada elección, en [`docs/LANGUAGE_STRATEGY.md`](docs/LANGUAGE_STRATEGY.md).

---

## Estructura del repositorio

```text
backend/
  apps/           puntos de entrada ejecutables (node, replay, signal_demo, backtest_demo)
  benchmarks/     programas de benchmark de latencia y throughput
  include/        cabeceras públicas compartidas
  modules/        implementación del backend por dominio
  schema/         definiciones de esquema/wire (FlatBuffers)
  tests/          objetivos de test del backend (38 targets de CTest)
docs/             arquitectura, roadmap, auditoría y documentación de preparación operativa
infra/            infraestructura local opcional (Docker Compose, init SQL de TimescaleDB)
scripts/          automatización del repositorio
```

Bibliotecas principales (todas bajo el prefijo técnico heredado `argentum_*` — ver nota de nombres más abajo): `argentum_core`, `argentum_bus`, `argentum_codec`, `argentum_network`, `argentum_datafeed`, `argentum_engine` (order book), `argentum_risk`, `argentum_trading` (OMS), `argentum_gateway` (ruteo/ejecución), `argentum_regime`, `argentum_persist`, `argentum_ev`, `argentum_strategy`, `argentum_signal`, `argentum_backtest`.

Ejecutables: `argentum_node` (runtime principal), `argentum_signal_demo` y `argentum_backtest_demo` (demos end-to-end), `argentum_replay` (utilidad de replay de journal), `argentum_pipeline_benchmark`, `argentum_matching_benchmark`, `argentum_regime_benchmark`.

> **Nota de nombres:** los nombres internos de binarios y librerías todavía usan el prefijo heredado `argentum_*`. El nombre de producto es **Axiotick**; el renombrado de los identificadores técnicos se hará en una fase separada, consciente de la compatibilidad hacia atrás.

---

## Construcción, pruebas y benchmarks

Requiere un compilador C/C++ compatible con CMake ≥ 3.20 (C++20, C17). En esta máquina el proyecto configura con Visual Studio 18 2026; Visual Studio 2022 también es compatible donde esté instalado.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Ejecutar el nodo del backend:

```powershell
.\build\bin\Release\argentum_node.exe
```

Correr los tests (CTest) después de compilar:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Correr los benchmarks (compilar Release primero):

```powershell
.\build\bin\Release\argentum_matching_benchmark.exe
.\build\bin\Release\argentum_pipeline_benchmark.exe
.\build\bin\Release\argentum_regime_benchmark.exe
```

Todo resultado de benchmark solo es útil si se registra junto con: CPU, memoria, SO y perfil de energía; compilador y flags de build; tamaño del dataset de entrada y esquema de eventos; percentiles p50/p95/p99/p99.9; throughput, drops, backpressure hits y profundidad de cola; y si persistencia, logging y rutas de API estaban habilitados durante la corrida.

Infraestructura local opcional (TimescaleDB vía Docker):

```powershell
docker compose -f infra/docker-compose.yml up -d
```

Documentación de API en C++: el repositorio incluye un `Doxyfile` en la raíz para generar documentación de las cabeceras vía Doxygen (`doxygen Doxyfile`).

---

## Hoja de ruta (roadmap)

El proyecto avanza en fases largas y verificables — cada fase es un portón de ingeniería, no un hito de marketing. Ninguna fase se considera completa sin documentación de estado actual, tests acordes a su perfil de riesgo, benchmarks donde la latencia o el throughput importen, criterios de aceptación explícitos, riesgos y dependencias conocidos, y evidencia de que los mocks están aislados de los caminos productivos.

| # | Fase | Estado |
|---|---|---|
| 00 | Auditoría y verdad del repositorio | Completa (baseline) |
| 01 | Núcleo de baja latencia y reproducibilidad de build | Parcial |
| 02 | Ingesta y normalización de datos de mercado | Parcial (orientado a replay) |
| 03 | Corrección y performance del order book | Parcial |
| 04 | Signal Engine (EV-Gate + régimen) | **Entregada** (Bloques 1-2) |
| 05 | Risk Engine | **Entregada parcialmente** (Bloque 1: kill switch diario; VaR runtime pendiente) |
| 06 | OMS y ejecución | En progreso |
| 07 | Backtesting institucional | **Entregada** (Bloques 2-3) |
| 08 | Paper trading | No iniciada |
| 09 | IA y plataforma de research | No iniciada (conceptual) |
| 10 | Observabilidad | No iniciada |
| 11 | Seguridad y gobernanza | No iniciada |
| 12 | Conectividad multi-exchange | No iniciada |
| 13 | Alta disponibilidad y producción | No iniciada |
| 14 | Empaquetado como producto financiable | No iniciada |

Hitos a un año:

- **30 días**: auditoría, build/tests/benchmarks reproducibles, separación de mocks, definiciones de métricas de latencia.
- **90 días**: OMS/riesgo/replay endurecidos, backtesting v1, base de paper trading.
- **180 días**: datos multi-fuente en modo paper, signal engine con trazabilidad completa, observabilidad, línea base de seguridad.
- **365 días**: plataforma piloto con portones de preparación para vivo controlados y documentación financiable.

Ver el detalle completo de cada fase en [`docs/roadmap/README.md`](docs/roadmap/README.md).

---

## Calidad, CI y contribución

### Principios técnicos

- La latencia es una métrica de producto, no una idea de último momento.
- Toda decisión de trading debe ser explicable y reproducible por replay.
- Ninguna orden se ejecuta sin pasar por el chequeo de riesgo.
- Research, backtesting, paper trading y live trading permanecen siempre separados.
- El valor esperado, los costos, el slippage, el spread, el funding y el drawdown importan más que el win rate crudo.
- Los mocks y simulaciones deben estar claramente marcados y aislados de los caminos de producción.
- Las afirmaciones de la documentación deben coincidir con el comportamiento implementado, o estar explícitamente etiquetadas como estado objetivo.
- Seguridad, auditabilidad y controles operativos son requisito previo a cualquier operación con capital real.

### Integración continua

El workflow de GitHub Actions (`.github/workflows/ci.yml`) corre en cada push y pull request: un job en `windows-latest` que configura, compila en Release y corre CTest; y un job en `ubuntu-latest` que valida formato con `clang-format` sobre todo `.c`/`.h`/`.cpp`/`.hpp` del repositorio (excluyendo directorios de build).

### Convenciones de Git

`main` siempre debe ser deployable. El trabajo ocurre en ramas de vida corta (`feature/<tema>`, `hotfix/<issue>`), los PRs requieren CI en verde y al menos una revisión para módulos core, se prefiere squash merge, y los mensajes de commit siguen el formato `<scope>: <resumen>`. Ver [`docs/GIT_STRATEGY.md`](docs/GIT_STRATEGY.md).

### Cómo contribuir

Las contribuciones deben mejorar corrección, medición de latencia, testeabilidad, auditabilidad o preparación para producción. Evitar features decorativas, afirmaciones de performance no verificadas, mocks ocultos y abstracciones que oscurezcan el comportamiento de trading o de riesgo. Antes de enviar cambios: compilar en modo Release, correr los tests relevantes, actualizar documentación cuando el comportamiento cambie, incluir evidencia de benchmark para cambios sensibles a latencia, y explicar el impacto en riesgo y las consideraciones de rollback.

---

## Aviso legal y de riesgo

Axiotick no es asesoramiento financiero. Este repositorio no constituye una recomendación de comprar, vender u operar ningún activo.

El objetivo de +60% de probabilidad de ROI alto descrito en [La tesis del producto](#la-tesis-del-producto) es una **meta de investigación e ingeniería**, no un resultado auditado ni una garantía de rentabilidad presente o futura. Ningún backtest, por riguroso que sea su diseño estadístico, garantiza resultados futuros — los mercados cambian de régimen, la liquidez cambia, y los costos reales de ejecución pueden diferir de los modelados.

Operar en vivo requiere validación independiente, límites de riesgo, gestión segura de claves, monitoreo operativo, kill switches, trazas de auditoría y supervisión humana — todo lo enumerado en el [checklist de preparación para live trading](docs/LIVE_TRADING_READINESS_CHECKLIST.md), sin excepciones. Ningún modelo de IA ni cuantitativo de este proyecto debe tratarse como un motor de certezas. Las señales deben interpretarse siempre en términos probabilísticos, y evaluarse a través de la disciplina de valor esperado, costos, riesgo y validación estadística descrita en este documento.
