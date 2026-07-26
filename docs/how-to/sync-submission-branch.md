# Como atualizar a branch de submissão

## Quando usar

Sempre que uma configuração nova foi validada localmente (score/p99 medidos com k6) e deve virar a versão que o avaliador da Rinha executa. A branch `submission` é o que o motor de teste clona e roda; `main` é o código-fonte. O histórico da edição 2026 mostra o padrão: 37 commits na `submission`, quase todos tocando apenas o `docker-compose.yml` (probes v0.24 → v0.27 e os "FINAL LOCK" de reversão).

## Pré-condições

| Condição | Como conferir |
|---|---|
| Imagem nova publicada no Docker Hub, pública, `linux-amd64` | `docker pull docker.io/nelsonsimionato/rinha-backend-2026:<tag>` de uma máquina sem login |
| Configuração validada localmente | k6 com checks 100% e `http_req_failed` zerado (passo 4 do GETTING_STARTED) |
| Branch `submission` conhecida localmente | `git fetch origin submission` |

A branch contém **somente** o necessário para rodar: `docker-compose.yml` na raiz, `info.json`, `LICENSE` (o `haproxy.cfg` presente é resquício da era HAProxy, anterior ao `lbfd`). Código-fonte não entra nela — o `docker-compose.yml` referencia imagens já publicadas.

## Passos

Execute no host, a partir da raiz do repositório:

```bash
git fetch origin
git worktree add ../rinha-submission submission
cd ../rinha-submission
```

Edite o `docker-compose.yml`: tags de imagem, variáveis de ambiente, `cpuset` e limites. Não adicione arquivos novos. Depois:

```bash
docker compose config --quiet && echo OK
```

Você deve ver `OK`. Confira também que a soma dos limites respeita a regra — `cpus` totalizando no máximo 1.0 e memória no máximo 350 MB entre os três serviços.

Suba a mudança com uma mensagem que registre a medição que a justifica (padrão do histórico: versão, p99 medido e o que reverter se regredir):

```bash
git commit -am "v0.XX: <mudança> — p99 <medido> local; revert para <commit> se regredir"
git push origin submission
```

Quando terminar, remova o worktree:

```bash
cd -
git worktree remove ../rinha-submission
```

## Verificação

Confirme que a branch remota tem exatamente o esperado:

```bash
git fetch origin && git ls-tree --name-only origin/submission
```

Você deve ver `LICENSE`, `docker-compose.yml`, `haproxy.cfg` e `info.json`, e `git show origin/submission:docker-compose.yml` deve exibir a configuração nova.

Para executar o teste oficial, abra uma issue com `rinha/test` na descrição, conforme `docs/spec/SUBMISSION.md` — o motor da Rinha roda o teste, comenta o resultado na issue e a fecha. O resultado cita o commit avaliado; confira que é o que você acabou de subir.

## Problemas comuns

| Sintoma | Causa | Correção |
|---|---|---|
| Avaliador falha ao puxar a imagem | Tag não publicada, repositório privado ou arquitetura errada | `docker push` da tag; conferir visibilidade pública e `linux-amd64` |
| Submissão rejeitada por limites | Soma de `cpus`/`memory` acima de 1 CPU / 350 MB | Reconferir `deploy.resources.limits` dos três serviços |
| Teste roda a configuração antiga | Push foi para `main` (ou outra branch), não para `submission` | `git push origin submission` e reconferir com `git show origin/submission:docker-compose.yml` |
| `api1`/`api2` nunca ficam `healthy` no avaliador | Imagem construída sem `resources/index.bin` embutido | Rodar `make index` antes do build da imagem (`c-impl/Dockerfile` aborta o build se o índice faltar) |
| Score regride no teste oficial | Probe que não transfere do ambiente local para o avaliador | Reverter para o último commit "LOCK" citado na mensagem do commit do probe |
