# Primeiros passos — Rinha de Backend 2026

## O que você vai conseguir

Ao final deste guia você terá o cluster completo da submissão (load balancer + 2 APIs) rodando localmente na porta 9999, terá recebido uma decisão de fraude via `POST /fraud-score` e terá validado o comportamento sob carga com k6. Duração estimada: **10 minutos**, dominados pelo download das imagens Docker.

Este caminho usa as imagens públicas já compiladas — não é preciso ter Go, gcc nem o dataset de 3M de vetores. A compilação local é assunto de docs/ARCHITECTURE.md.

## Pré-requisitos

| Ferramenta | Versão | Como verificar |
|---|---|---|
| git | qualquer recente | `git --version` |
| Docker Engine | com suporte a `cpuset` no Compose | `docker --version` |
| Docker Compose (plugin v2) | v2.x | `docker compose version` |
| curl | qualquer recente | `curl --version` |
| k6 | qualquer recente | `k6 version` |

As imagens são `linux-amd64`; em outra arquitetura o Docker precisará de emulação. O `cpuset` do compose pressupõe uma máquina com pelo menos 4 CPUs lógicas (cores 0 a 3 são usados).

## 1. Instalação

Execute no host:

```bash
git clone https://github.com/nelsonsimionato/rinha-backend-2026.git
cd rinha-backend-2026
```

Você deve ver, entre outros, os arquivos `docker-compose.yml`, `c-impl/` e `k6/` ao rodar `ls`.

## 2. Primeira execução

Nenhuma configuração é necessária — o `docker-compose.yml` da raiz já aponta para as imagens publicadas, com o índice de busca embutido.

Execute no host:

```bash
docker compose up -d
```

O Compose baixa as imagens, sobe `api1` e `api2`, espera os healthchecks e então sobe o `lb`. Confirme com:

```bash
docker compose ps
```

Você deve ver os três serviços com `STATUS` contendo `Up`, e `(healthy)` em `api1` e `api2`.

## 3. Primeira requisição

Execute no host:

```bash
curl -i http://localhost:9999/ready
```

Você deve ver a primeira linha `HTTP/1.1 200 OK`.

Agora envie uma transação real (o primeiro payload de `resources/example-payloads.json`):

```bash
curl -s -X POST http://localhost:9999/fraud-score \
  -H 'Content-Type: application/json' \
  -d '{
    "id": "tx-1329056812",
    "transaction": { "amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z" },
    "customer": { "avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003", "MERC-016"] },
    "merchant": { "id": "MERC-016", "mcc": "5411", "avg_amount": 60.25 },
    "terminal": { "is_online": false, "card_present": true, "km_from_home": 29.23 },
    "last_transaction": null
  }'
```

Este é o payload canônico legítimo da especificação; você deve ver exatamente:

```json
{"approved":true,"fraud_score":0.0000}
```

## 4. Verificação sob carga

Execute no host, na raiz do repositório:

```bash
k6 run k6/test.js
```

O cenário dispara 5.000 iterações com 50 usuários virtuais contra `http://localhost:9999/fraud-score`. Você deve ver, no resumo final do k6, os dois checks com 100% de sucesso:

```
✓ status is strictly 200 OK
✓ has approved property
```

e `http_req_failed` com `rate==0.00` aprovado. Se isso apareceu, o cluster está íntegro e respondendo dentro do contrato do desafio.

## 5. Encerramento

Quando terminar, derrube o cluster e os volumes:

```bash
docker compose down -v
```

Você deve ver as mensagens `Removed` para os três containers, a rede e o volume `sock`.

## Próximos passos

- Entenda como a requisição percorre o LB de passagem de FD, o parser e a KD-tree → docs/ARCHITECTURE.md
- Compile a solução a partir do código-fonte e rode os testes unitários de `c-impl/tests/` → seção "Build local" em docs/ARCHITECTURE.md
- Ajuste parâmetros de latência (`EPOLL_IDLE_US`, `SPIN_US`, cpuset) → TUNING.md e docs/ARCHITECTURE.md
- Promova uma configuração validada para a branch que o avaliador executa → docs/how-to/sync-submission-branch.md
- Leia a especificação oficial do desafio → docs/spec/README.md
