# 0005 — Load balancer próprio com passagem de FD (single-hop)

- **Status**: Aceita
- **Data**: 2026-06-02 (aprox., commit `16d7982`, v0.18)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

A regra do desafio exige um load balancer round-robin na frente de 2 instâncias, mas proíbe lógica de detecção nele — o LB é puro overhead obrigatório. Com HAProxy (usado até a v0.17, config em `haproxy.cfg`), cada byte de cada requisição atravessa dois hops de proxy (cliente→LB→API e volta), pagando cópias de userspace e trocas de contexto num orçamento de 0,10 CPU. Com o compute por query já na casa das dezenas de microssegundos, o proxy virou parcela relevante do p99 sub-milissegundo.

## Decisão

Vamos escrever um LB mínimo em C (`c-impl/lb/lbfd.c`) que faz o round-robin **por conexão**, não por requisição: `accept4()` do TCP do cliente na :9999 e entrega do file descriptor à API escolhida via `SCM_RIGHTS` sobre um socket AF_UNIX em volume compartilhado. A API (`server.c`, canal `FDSOCK`) adota o socket e serve o cliente diretamente pela vida da conexão keep-alive. O LB fica fora do caminho dos dados.

## Consequências

- Zero hops de proxy no caminho dos dados: depois do handoff, os bytes fluem cliente↔API como se não houvesse LB. Commit `16d7982` nomeia o objetivo: "chase sub-ms p99".
- O round-robin passa a ser por conexão nova — compatível com a regra do desafio e com o padrão do avaliador (k6 com keep-alive e pool fixo de conexões, que se distribui uniformemente).
- Tudo continua em rede `bridge` (o handoff usa socket unix em volume, não a rede) — dentro das regras de infraestrutura.
- O LB precisa de imagem própria (`c-impl/lb/Dockerfile.fd`, publicada como `v0.18-lbfd`) e as APIs de um canal extra de accept; a complexidade vive em ~200 linhas de C auditáveis.
- Rebalanceamento fino por requisição deixa de existir; com 2 instâncias homogêneas e carga uniforme, o custo é nulo na prática.

## Alternativas consideradas

- **HAProxy 2.8** — rejeitada para a submissão: mesmo com `maxconn` ajustado, `nbthread 1` e logs silenciados (`haproxy.cfg`), continua um proxy de dois hops; mantido na topologia de desenvolvimento (`c-impl/docker-compose.yml`).
- **Proxy próprio com `splice()`** (`c-impl/lb/lb.c`) — implementado e preterido: zero-copy elimina cópias de userspace, mas mantém o LB em cada byte trafegado; permanece no repositório como alternativa.
