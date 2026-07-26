# Arquitetura da solução

Este documento descreve a arquitetura da submissão em C — a que está implantada pelo `docker-compose.yml` da raiz. A visão panorâmica (C4 níveis 1 e 2) está no README; aqui está o detalhe: topologias, o caminho da requisição, os módulos, o formato do índice e as táticas de latência. As motivações de cada escolha estão em `docs/adr/`.

A especificação do desafio (contrato da API, fórmulas de vetorização, regras de pontuação) está em `docs/spec/` e não é repetida aqui.

## Objetivo de projeto

A pontuação da Rinha soma dois componentes independentes, cada um valendo até +3000: latência (satura com p99 ≤ 1 ms) e detecção. A arquitetura persegue os dois tetos ao mesmo tempo:

- **Detecção**: busca 5-NN **exata** — o resultado é, por construção, o mesmo que o gabarito do avaliador calcula, exceto pelo erro de quantização int16 (ver ADR 0004).
- **Latência**: eliminar trabalho por requisição (proxy, parsing genérico, alocação) e eliminar latência de agendamento (spin, warmup, cpuset).

Resultado medido no ambiente local: **6000 pontos, p99 ≈ 0,42 ms**. Resultado final oficial: **6000 pontos, p99 0,4581 ms, 4º lugar geral** (tabela completa no README).

## Duas topologias de execução

| | Submissão (`docker-compose.yml` da raiz) | Desenvolvimento (`c-impl/docker-compose.yml`) |
|---|---|---|
| Imagens | Publicadas no Docker Hub (`nelsonsimionato/rinha-backend-2026:v0.24-lbfd` e `:v0.25-lean`) | Build local (`c-impl/Dockerfile`, contexto na raiz do repo) |
| Load balancer | `lbfd` (passagem de FD, `DEFER_ACCEPT=1`) | HAProxy 2.8-alpine (`c-impl/haproxy.cfg`) |
| Índice | Embutido na imagem | Copiado de `resources/index.bin` — exige `make index` antes |
| Limites | `cpus` 0.49/0.49/0.02, mem 160/160/30 MB, `cpuset` 0/1/"2,3" | `cpu_period` 10 ms + `cpu_quota` (0.45/0.45/0.10), mem 160/160/30 MB |
| Uso | Espelho da configuração final avaliada | Iterar no código C sem publicar imagem |

O período CFS de 10 ms na topologia de desenvolvimento (contra os 100 ms default) limita qualquer janela de throttle a 10 ms, o que protege o p99 quando a quota estoura.

Há ainda uma terceira ponta: a **branch `submission`**, que contém apenas os arquivos que o avaliador executa (`docker-compose.yml` na raiz, `info.json`, `LICENSE`). A configuração final travada nela (commit `dbebe2b`, v0.25) é a que o compose da raiz espelha; o código correspondente (linha v0.24–v0.27, desenvolvida na branch `v0.24-keepwarm`) foi mergeado na `main` — a v0.27 (busca SoA, `SEARCH_V2`) está no código mas ficou fora da submissão, desligada por default. O processo de promoção está em `docs/how-to/sync-submission-branch.md`.

## O caminho da requisição

```mermaid
sequenceDiagram
    participant C as Cliente (k6)
    participant L as lbfd (cpus 2,3)
    participant A as api1 ou api2 (cpu 0 ou 1)

    Note over L,A: Na subida: cada api cria seu socket AF_UNIX<br/>(/sock/apiN.sock, volume compartilhado); o lbfd conecta com retry
    C->>L: TCP connect :9999
    L->>A: FD do cliente via SCM_RIGHTS (round-robin por conexão)
    Note over L: O lbfd sai do caminho — não lê nem escreve payload
    C->>A: POST /fraud-score (direto, keep-alive)
    A->>A: parse HTTP → parse JSON → vetoriza (14 dims, i16) → 5-NN na KD-tree
    A->>C: {"approved":…,"fraud_score":…} (um único write, resposta pré-formatada)
```

Pontos que definem o custo por requisição:

1. **Zero hops de proxy.** O `lbfd` (`c-impl/lb/lbfd.c`) só participa do estabelecimento: `accept4()` na :9999 e repasse do FD via `SCM_RIGHTS` pelo socket unix. Toda a vida útil da conexão keep-alive é servida diretamente pela API. O balanceamento é round-robin **por conexão nova**, como a regra do desafio exige.
2. **Servidor de evento único.** Cada API é um processo de uma thread com epoll (`server.c`) — level-triggered para conexões de clientes desde a v0.25 ("lean path": mapa de FD O(1), leitura em um único `recv`), edge-triggered apenas no canal de handoff do LB — sem alocação no caminho quente. A API também mantém um listener TCP próprio (porta 8080) usado pelo healthcheck `/ready`; o canal `FDSOCK` é adicional.
3. **Parsing especializado.** `http.c` reconhece apenas as duas rotas do contrato; `json.c` decodifica apenas o shape do payload de fraude, guardando strings como fatias do buffer de leitura (zero cópia). Payload malformado nunca vira 4xx/5xx — vira a resposta fallback `approved:true, fraud_score:0.0` (erro HTTP pesa mais que falso negativo na pontuação).
4. **Resposta pré-formatada.** Só existem 6 respostas possíveis (`fraud_count` 0..5). `response.c` mantém as 6 já serializadas com headers + body no mesmo buffer: um único `write()` por resposta.

## Vetorização

`vectorize.c` implementa as fórmulas de `docs/spec/DETECTION_RULES.md`: 14 dimensões normalizadas para [0,1], com `-1` para as dimensões 5 e 6 quando `last_transaction` é nula. Particularidades da implementação:

- **Quantização i16 escala 10000**: [0,1] → [0,10000]; o sentinela −1 → −10000 (`I16_SENTINEL`). Overflow-safe: o pior `dist_sq` é ~2,0e9 < `INT32_MAX` (análise em `compat.h`).
- **Stride 16**: cada vetor ocupa 16 lanes int16 (14 dims + 2 de padding zero) = 32 bytes, exatamente um registrador ymm.
- **Risco por MCC compilado**: a tabela de `mcc_risk.json` vira um `switch` em `mcc_risk()` (`vectorize.c`) — o runtime C não lê JSON algum na subida, só o `index.bin`.
- **Datas sem libc**: `time_math.c` faz parse de ISO-8601 fixo, dia-da-semana por congruência de Zeller e diferença de minutos com aritmética de dias — byte a byte compatível com o builder Go que rotulou o índice.

## Busca: KD-tree exata com kernels AVX2

`search.c` faz 5-NN exato por descida best-first com poda por AABB:

- Cada nó da árvore carrega a bounding box (min/max por dimensão) da sua subárvore; o kernel `bound_dist_sq` (assembly AVX2 em `distance.s`) calcula a distância mínima possível da query até a caixa e descarta subárvores que não podem melhorar o pior dos 5 atuais.
- Folhas de 32 registros são varridas com o kernel de lote (`distance_batch.s`, `vpsubw` + `vpmaddwd`, 4 vetores por iteração).
- Custo típico: **~244 registros escaneados por query** contra 3.000.000 do brute force (comentário-medida no `Makefile` da raiz).
- A recall é exata **em relação aos vetores quantizados** — não há parâmetro de probe/recall para ajustar.

## O índice (formato v14)

Gerado offline por `tools/build_kdtree.go` (`make index`), consumido por `index.c` via `mmap` somente-leitura:

| Offset | Conteúdo |
|---|---|
| `[0]` | `formatVersion` (uint8) = 14 — verificado na subida; divergência aborta o processo |
| `[4:8]` | `N` (uint32) — total de registros |
| `[8:12]` | `nodeCount` (uint32) |
| `[16:…]` | `nodes[nodeCount]` × 72 bytes: `bmin[16]` i16, `bmax[16]` i16, `a` u32, `b` u32 |
| `[…]` | `data[N][16]` int16 — vetores reordenados na ordem das folhas |
| `[…]` | `isFraud[N]` uint8 — mesma ordem |

Nó interno: `a`/`b` = índices dos filhos. Folha: bit alto de `b` ligado (`KD_LEAF_FLAG`), `a` = primeiro registro, `b & KD_COUNT_MASK` = contagem. Split por sliding-midpoint na dimensão de maior spread, folha de 32 registros.

O arquivo tem ~118 MB e é imutável durante o teste — por isso é embutido na imagem em build time.

## Táticas de latência de cauda

O compute por requisição é da ordem de dezenas de microssegundos; o p99 passa a ser dominado por latência de agendamento (wakeup, idle profundo de core, roubo de CPU). As contramedidas, todas configuráveis por ambiente:

| Variável | Onde é lida | Default | Efeito |
|---|---|---|---|
| `EPOLL_IDLE_US` | `server.c` | 0 (desligado) | Keep-warm: bloqueia em `epoll_pwait2` com timeout de N µs e re-bloqueia no timeout — o core nunca prevê ociosidade longa, fica no C-state raso C1 com clock alto; custo limitado (~2–3% de CPU) independente da carga. Fallback para `epoll_wait` em ms se `epoll_pwait2` não existir |
| `SPIN_US` | `server.c` | 0 (desligado) | Busy-poll de até N µs antes de bloquear no epoll — captura a próxima requisição sem pagar wakeup do scheduler |
| `EPOLL_TIMEOUT_MS` | `server.c` | −1 (bloqueia) | `1` acorda o loop a cada 1 ms — usado como fallback quando o keep-warm está desligado ou indisponível |
| `BUSY_POLL_US` | `server.c` | 0 (desligado) | NAPI busy-poll integrado ao epoll (kernel ≥ 6.9); best-effort — sem privilégio costuma ser no-op |
| `WARMUP_QUERIES` | `main.c` → `server_warmup` | 0 | N buscas sobre vetores do índice antes de servir — aquece branch predictor, I-cache e dados quentes |
| `FDSOCK` | `server.c` | vazio (desligado) | Caminho do socket unix onde a API recebe FDs do `lbfd` |
| `DEFER_ACCEPT` | `lb/lbfd.c` | 0 (desligado) | `TCP_DEFER_ACCEPT` no listener do LB: o accept só acorda quando os primeiros bytes chegaram — o FD repassado já está legível |
| `INDEX_HUGE` / `INDEX_MLOCK` | `index.c` | desligados | THP (`madvise`) e `mlock` no índice mapeado — sondagem v0.26, não usada na submissão |
| `SEARCH_V2` | `index.c` | desligado | Kernel de busca SoA por chunks de folha (v0.27) — exato, mas não transferiu ganho no avaliador; base para a próxima edição |
| `PORT` | `main.c` | 8080 | Porta do listener TCP próprio (healthcheck) |
| `INDEX_PATH` | `main.c` | `/resources/index.bin` | Caminho do índice |

A configuração final avaliada roda com `EPOLL_IDLE_US=60`, `SPIN_US=0`, `EPOLL_TIMEOUT_MS=1`, `BUSY_POLL_US=5`, `WARMUP_QUERIES=5000` e, no LB, `DEFER_ACCEPT=1` (ADR 0007).

**Layout de cpuset (o maior alavancador medido):** `cpuset` é posicionamento, não quota — não conta contra o limite de 1 CPU. O layout `1/2/3` (cpu 0 livre para SO/IRQs) foi o que destravou o 6000 local na v0.23; a configuração final convergiu para **APIs nos cpus 0/1 (um por core físico) e LB em "2,3" (os irmãos de hyperthread, 0,02 CPU)** — o layout de consenso das entradas mais rápidas do leaderboard. O racional está no cabeçalho do `docker-compose.yml` e no ADR 0007.

## Pipeline offline (Go)

As ferramentas em `tools/` rodam com `//go:build ignore` — são executadas via `go run`, nunca linkadas na API:

| Ferramenta | Comando | Produz |
|---|---|---|
| `build_kdtree.go` | `make index` (ou `go run tools/build_kdtree.go`) | `resources/index.bin` a partir de `resources/references.json.gz` |
| `mccgen.go` | `make generate` | `mcc_data.go` (tabela MCC para a implementação Go legada) |
| `synth_refs.go` | `go run tools/synth_refs.go -n <N>` | `references.json.gz` sintético, para quem não tem o dataset oficial |
| `inspect_bin.go` | `go run tools/inspect_bin.go` | Inspeção/validação de um `index.bin` |
| `build_partition_hash.go` | — | Builder da estratégia anterior (partição por feature-hash); mantido como referência |

`resources/references.json.gz` (3M de registros, ~50 MB) e `resources/index.bin` não são versionados (ver `.gitignore`); o primeiro vem da organização do desafio ou do `synth_refs.go`.

## Build local e testes

```bash
# API C (na pasta c-impl/)
make            # gera o binário estático `api`
make test       # compila e roda tests/test_{distance,json,kd_exact,search,vectorize}

# Load balancers (na pasta c-impl/lb/)
make            # gera `lb` e `lbfd`

# Cluster de desenvolvimento (na pasta c-impl/, exige resources/index.bin)
docker compose up --build -d
```

Os testes unitários linkam contra os objetos de `src/` (exceto `main.o`) e cobrem os kernels de distância, o parser JSON, a exatidão da KD-tree contra brute force, a busca e a vetorização.

Alvos do `Makefile` da raiz: `generate`, `index`, `up`, `down`, `test` (k6), `all` (pipeline completo) e `clean-host` — este último **para e remove todos os containers Docker do host** para isolar capacidade de hardware; use com atenção fora de máquina dedicada.

## A implementação Go legada

`main.go` + `distance_amd64.s` na raiz são a implementação original (fasthttp, partição por feature-hash, formato v11), superada pela reescrita em C a partir da v0.13 (ADR 0002). O código permanece como registro histórico e não é implantado; o `Dockerfile` da raiz ainda a compila. `AUDIT_FINDINGS.md` e `IMPLEMENTATION_PLAN.md` documentam a auditoria que motivou o redesenho.

## Limites conhecidos e débito técnico

- `MAX_CONNECTIONS` = 256 por API (`compat.h`) — dimensionado para os 50 VUs do avaliador, não para uso geral.
- Corpo máximo de request: 4096 bytes (`MAX_BODY_SIZE`).
- Comentários defasados citam formatos v12/v13 (`Makefile` da raiz, `index.h`, `build_kdtree.go`); a autoridade é `FORMAT_VERSION = 14` em `compat.h`, validado na carga do índice.
- `info.json` da raiz declara a stack da inscrição original (`go, fasthttp, fastjson, haproxy`) e permanece congelado por regra de inscrição; a cópia que o avaliador lê é a da branch `submission`, essa sim atualizada (`c, epoll, fd-passing-lb`).
- A API assume x86-64 com AVX2 (Haswell+); não há fallback portátil no runtime C.
