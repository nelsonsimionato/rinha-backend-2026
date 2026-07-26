# 0007 — Keep-warm por `epoll_pwait2` e a configuração final v0.25

- **Status**: Aceita (refina as táticas 2 e 3 do ADR 0006 para a configuração final)
- **Data**: 2026-06-05 (commits `4511442` v0.24, `10a8ae3` v0.25 e `dbebe2b` FINAL LOCK)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

Com o ADR 0006, o p99 local estabilizou em ~0,42 ms — mas as entradas do topo do preview rodavam em ~0,30 ms. O delta não era compute: era o mecanismo de manter o core quente. O poll de 1 ms (`EPOLL_TIMEOUT_MS=1`) ainda deixa o governador de cpuidle prever janelas de ociosidade longas o bastante para descer a C-states profundos (saída de C3/C6 custa ~30–100 µs em Haswell) e derrubar o clock; e o `SPIN_US=50` em todo tick ocioso queima quota de CFS que escala com o tempo, não com a carga — matemática suicida se o script final for mais pesado. Além disso, no LB, o accept de conexão nova acordava duas vezes (conexão e primeiros bytes), criando uma cauda de ~14 ms em conexões novas.

## Decisão

Vamos adotar, para a configuração final (imagens `v0.25-lean` + `v0.24-lbfd`):

1. **Keep-warm** (`EPOLL_IDLE_US=60`): bloquear em `epoll_pwait2` com timeout de 60 µs e re-bloquear a cada timeout sem eventos. O core nunca prevê mais que 60 µs de ociosidade → o cpuidle fica no C1 raso e o `intel_pstate` mantém o clock. Custo ~2–3% de CPU, **invariante à carga** — não pode estourar a quota do CFS por mais pesado que seja o script. `PR_SET_TIMERSLACK` apertado para o timeout de 60 µs não esticar. Fallback para `epoll_wait` em ms se `epoll_pwait2` não existir (`ENOSYS`).
2. **`SPIN_US=0` em ticks ociosos**: o spin passa a ocorrer só logo após eventos processados — a v0.25 usa *menos* CPU que a v0.23 no total.
3. **`TCP_DEFER_ACCEPT` no LB** (`DEFER_ACCEPT=1`): o accept só acorda quando os primeiros bytes do request chegaram — o FD repassado à API já está legível, eliminando o duplo wakeup de conexão nova.
4. **Layout APIs cpu 0/1 + LB "2,3"** (0,49/0,49/0,02): um core físico inteiro por API, LB nos irmãos de hyperthread — o layout de consenso do 1º e 2º colocados do preview.
5. **Caminho de serviço "lean"** (v0.25): epoll level-triggered para clientes, mapa de FD O(1), leitura em um único `recv`.

## Consequências

- Resultado oficial final: **6000/6000, p99 0,4581 ms, 4º lugar geral**, zero erros em 50.000 requisições — dentro da banda validada localmente (0,394–0,400 ms no avaliador de preview).
- O poll de 1 ms do ADR 0006 vira fallback; o spin em tick ocioso é abandonado. Warmup e a lição "layout importa mais que pinning" permanecem.
- Duas sondagens posteriores foram **revertidas por medição no avaliador** antes do lock: THP+mlock do índice (v0.26, `41aaa49`) e o kernel de busca SoA (v0.27, `7150ab5`) — este exato e ~12% mais rápido localmente, mas o ganho de ~6 µs não transferiu no p99 do avaliador (cauda dominada por memória e ruído). Ambos permanecem no código desligados por env (`INDEX_HUGE`/`INDEX_MLOCK`, `SEARCH_V2`).

## Alternativas consideradas

- **Aumentar `SPIN_US`** — rejeitada: queima de quota proporcional ao tempo ocioso; sob o script final (mais pesado), risco de throttle do CFS exatamente na hora errada.
- **`SO_BUSY_POLL`/NAPI agressivo** — mantido apenas como best-effort (`BUSY_POLL_US=5`): sem privilégio é tipicamente no-op no avaliador, e localmente chegou a piorar.
- **Manter o layout `1/2/3`** — rejeitada por medição: a banda da v0.25 com APIs 0/1 + LB "2,3" ficou abaixo do melhor resultado do layout anterior (0,414 ms).
