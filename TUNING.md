# Tuning playbook

Cheatsheet de quais parâmetros afetam quais métricas e qual o custo de mexer em cada um.
Útil quando o teste oficial volta um score abaixo do esperado.

## Mapa de impacto

| Parâmetro | Onde | Tipo de mudança | Efeito esperado |
|---|---|---|---|
| `IVF_PROBE` | env var no `docker-compose.yml` ou shell | **sem rebuild** — só restart do container | ↑ probe → ↑ recall, ↑ p99. Range 1..32 (limite hard em `IvfProbeMax` em `main.go`). |
| `IVF_K` | `Makefile` (default 2048) | rebuild do índice (offline, ~1 min/iter) + rebuild da imagem | ↑ K → clusters menores, ↑ centroid-scan cost, recall similar pro mesmo % de probe |
| `IVF_ITERS` | `Makefile` (default 5) | só rebuild do índice (offline) | ↑ iters → clusters mais uniformes → recall efetivo maior pelo mesmo probe |
| HAProxy timeouts | `haproxy.cfg` | só restart do container haproxy | timeouts mais agressivos = falhar rápido em vez de bloquear; mas pode aumentar erro rate |
| Resource limits (CPU/RAM) | `docker-compose.yml` | restart | mais CPU/instância → menor p99 sob carga, mas budget é fixo (1 CPU / 350 MB total) |
| `K` (k-NN k) | `main.go` constant | rebuild da imagem | **NÃO MUDE** — spec da Rinha fixa em 5 |
| `IvfProbeMax` | `main.go` constant | rebuild da imagem | aumenta só se quiser passar de probe=32 (a partir de 64 começa a ficar caro em CPU/mem) |

## Workflows típicos

### A. "Recall baixo na detection, p99 com folga"

Solução barata: aumentar `IVF_PROBE` sem rebuild.

```bash
cd /home/nelsinho/projetos/rinha_2026
IVF_PROBE=24 docker compose up -d --force-recreate
# Roda k6 local pra confirmar p99 ainda OK
k6 run k6/test.js
```

Se p99 continuar OK e recall melhorar, persistir mudando o default no `docker-compose.yml`:
```yaml
environment:
  - IVF_PROBE=${IVF_PROBE:-24}    # mudou de 16 para 24
```

Rebuild da imagem? **Não precisa** — `IVF_PROBE` é só env. Mas se quiser que o novo default vá pro test env oficial, é só commit + push do compose na branch `submission`. O ambiente da Rinha vai puxar a próxima vez que rodar.

### B. "Recall ainda baixo mesmo com probe=32"

Significa que os clusters não estão capturando bem a vizinhança real. Solução: rebuild do índice com K maior ou mais iters.

```bash
cd /home/nelsinho/projetos/rinha_2026
rm resources/index.bin
make index IVF_K=4096 IVF_ITERS=5   # K=4096 dá ~730 records/cluster vs 1464 antes

# Rebuild da imagem Docker (índice está dentro dela)
docker buildx build --platform linux/amd64 \
    -t nelsonsimionato/rinha-backend-2026:latest \
    --push .

# Próximo teste no ambiente da Rinha vai puxar imagem nova
```

Cuidado: K=4096 dobra centroid-scan cost por query (4096 distâncias). Combinado com probe=8 (vs probe=16 antes) pode dar recall similar com menos CPU/req.

### C. "p99 alto, recall OK"

Tente reduzir o probe. Cada probe a menos elimina ~1464 distâncias por query (com K=2048).

```bash
IVF_PROBE=12 docker compose up -d --force-recreate
k6 run k6/test.js
```

Se recall ainda passar o teste de detection do oficial mas p99 cair, ótimo.

### D. "Erros (HTTP 5xx ou timeout) > 0"

Já temos `defer recover()` que evita 500 por panic. Se ainda ver erros:

1. Aumentar memory limits dos containers (custa do budget total 350 MB):
   ```yaml
   memory: 160M   # +10 MB cada api
   ```
   Mas cuidado: total atual é 50+150+150 = 350 MB exato. Sem margem. Se precisar mais memory pra api, reduz haproxy:
   ```yaml
   haproxy: memory 40M  →  api1/api2: memory 155M cada
   ```

2. Verificar logs:
   ```bash
   docker logs rinha_2026-api1-1 --tail 50
   docker logs rinha_2026-api2-1 --tail 50
   ```

3. Tighten timeouts no `haproxy.cfg` se algumas requests estão pendurando:
   ```
   timeout server 5s    # de 30s para 5s — falha rápido vez de bloquear
   ```

## Iteração padrão (em ciclos)

1. **Disparar preview test**:
   ```bash
   gh issue create --repo zanfranceschi/rinha-de-backend-2026 \
       --title "Preview test: nelsonsimionato (iter N)" \
       --body "rinha/test"
   ```

2. **Esperar score** (varia, fila do bot).

3. **Diagnosticar** com base no resultado:
   - Detection ruim, p99 OK → workflow A (mais probe)
   - Detection OK, p99 alto → workflow C (menos probe)
   - Ambos ruins → workflow B (rebuild de índice melhor)
   - Erros > 0 → workflow D

4. **Aplicar** mudança apropriada.

5. **Rebuild local + push imagem** (se mexeu em código/índice):
   ```bash
   docker buildx build --platform linux/amd64 \
       -t nelsonsimionato/rinha-backend-2026:latest --push .
   ```

6. **Atualizar tag se virar marco**:
   ```bash
   git tag -a v0.X-experimento -m "..."
   git push origin v0.X-experimento
   ```

7. **Voltar pro passo 1** (nova issue de preview test).

## Como reverter pra baseline submetido

```bash
cd /home/nelsinho/projetos/rinha_2026
git checkout v0.1-submitted
make index    # rebuilda índice com config baseline
docker buildx build --platform linux/amd64 \
    -t nelsonsimionato/rinha-backend-2026:latest --push .
git checkout main   # volta pra branch principal pra continuar mexendo
```

Tag `v0.1-submitted` aponta pro commit que gerou a imagem com `digest sha256:2fc1bf75…`.
