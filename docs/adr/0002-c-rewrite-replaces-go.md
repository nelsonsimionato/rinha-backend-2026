# 0002 — Reescrita do runtime em C, substituindo a implementação Go

- **Status**: Aceita
- **Data**: 2026-05-24 (aprox., commit `523051b`, v0.13)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

A implementação original (`main.go`, fasthttp + fastjson) já usava assembly AVX2 (`distance_amd64.s`) e foi extensamente ajustada para o orçamento de 0,40–0,45 vCPU por instância: `GOMAXPROCS=1`, `GOGC=200` e `GOMEMLIMIT` no `Dockerfile` da raiz, com comentários registrando a briga contra threads do runtime disputando a quota e poluindo o working set de ~45 MB em L2/L3. Com a pontuação de latência saturando apenas em p99 ≤ 1 ms, qualquer jitter de scheduler ou GC no caminho da requisição vira teto de score.

## Decisão

Vamos reescrever o runtime de serviço em C (`c-impl/`): servidor HTTP próprio sobre epoll edge-triggered, single-thread, parser JSON especializado, respostas pré-formatadas, binário estático em imagem `scratch`, mantendo os kernels de distância em assembly AVX2. O commit `523051b` ("v0.13: C rewrite with AVX2 ASM and epoll edge-triggered HTTP server") materializa a decisão.

## Consequências

- O caminho da requisição fica sem GC, sem scheduler de goroutines e sem alocação — o p99 passa a depender só do algoritmo e do agendamento do SO (atacado depois pelo ADR 0006).
- O time perde a biblioteca padrão de Go no runtime: HTTP, JSON e datas são reimplementados à mão (`http.c`, `json.c`, `time_math.c`) e testados contra o comportamento do builder Go (`make test` em `c-impl/`).
- Go permanece no projeto como linguagem do tooling offline (`tools/`), onde latência não importa.
- A implementação Go fica na raiz como registro histórico, não implantada.

## Alternativas consideradas

- **Continuar ajustando o runtime Go** — rejeitada: os ajustes de `GOMAXPROCS`/`GOGC`/`GOMEMLIMIT` já estavam feitos e o custo residual do runtime continuava presente no p99.
- **Outras linguagens sem runtime gerenciado** — não há evidência no repositório de que tenham sido avaliadas; C foi o caminho natural dado que os kernels críticos já estavam em assembly.
