# 0006 — Táticas de latência de agendamento: warmup, poll periódico, spin e cpuset

- **Status**: Aceita
- **Data**: 2026-06-04 (aprox., commits `d331aec` v0.21, `9c981e8` v0.22 e `ffa3390` v0.23)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

Depois dos ADRs 0001–0005, o compute por requisição ficou na casa de dezenas de microssegundos — mas o p99 medido continuava acima do teto de 1 ms. O residual não era algoritmo: era latência de agendamento do sistema — cores entrando em idle profundo entre requisições, custo de wakeup do scheduler ao sair do `epoll_wait`, e disputa das APIs com IRQs e com o próprio gerador de carga pelos mesmos cores. Em um servidor que responde em ~20 µs, um único wakeup de centenas de µs domina a cauda.

## Decisão

Vamos atacar o agendamento em quatro frentes, todas configuráveis por ambiente (leitura em `c-impl/src/server.c` e `main.c`, valores da submissão no `docker-compose.yml` da raiz):

1. **Warmup na subida** (`WARMUP_QUERIES=5000`): buscas sobre vetores do próprio índice antes de aceitar tráfego, aquecendo branch predictor, I-cache e dados quentes.
2. **Poll periódico** (`EPOLL_TIMEOUT_MS=1`): o `epoll_wait` deixa de bloquear indefinidamente e acorda a cada 1 ms, mantendo o core fora de estados de idle profundos.
3. **Spin antes de bloquear** (`SPIN_US=50`): busy-poll de até 50 µs antes de dormir no epoll — a próxima requisição chegando nessa janela é atendida sem pagar wakeup.
4. **Layout de cpuset `1/2/3`**: `api1`→cpu 1, `api2`→cpu 2, `lb`→cpu 3, cpu 0 livre para SO/IRQs e para o gerador de carga. `cpuset` é posicionamento, não quota, e não conta contra o limite de 1 CPU do desafio.

## Consequências

- No ambiente de teste local, o conjunto levou a solução ao **score máximo (6000), p99 ≈ 0,42 ms** — o cpuset foi a alavanca decisiva, e o *layout* importa mais que o pinning em si: a variante `0/1/2,3` (v0.20) regrediu por colocar `api1` no core carregado de IRQs e o LB nos irmãos de hyperthread das APIs (racional no cabeçalho do `docker-compose.yml`).
- Spin e poll periódico gastam CPU deliberadamente em troca de cauda: são seguros dentro da quota porque o servidor é single-thread e a carga do avaliador é contínua; em um serviço de carga esparsa, seriam desperdício.
- Todos os parâmetros são env vars — reversíveis com restart de container, sem rebuild de imagem.

## Alternativas consideradas

- **Deixar o scheduler decidir o posicionamento** (sem cpuset) — rejeitada: era exatamente a fonte da variância de cauda; a migração de processos entre cores destrói caches quentes.
- **Aumentar agressivamente a janela de spin** — rejeitada: com quotas de 0,45 CPU, spin longo demais consome a quota e provoca throttling do CFS, invertendo o efeito.
