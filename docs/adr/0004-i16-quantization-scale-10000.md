# 0004 — Quantização int16 com escala 10000

- **Status**: Aceita (substitui a quantização uint8 da implementação original)
- **Data**: 2026-06-01 (aprox., commits `31eed90` escala 5000 e `c4814a6` escala 10000, v0.17)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

A implementação original quantizava as 14 dimensões em uint8 (256 níveis). Como a decisão approve/deny fica a um vizinho de distância do limiar (`fraud_score < 0.6` com 5 vizinhos), erros de arredondamento perto de fronteiras de decisão trocam vizinhos do top-5 e viram falsos positivos/negativos — a quantização uint8 tinha se tornado o teto da pontuação de detecção. Ao mesmo tempo, os kernels SIMD precisam de um tipo inteiro estreito: o par `vpsubw` + `vpmaddwd` processa 16 lanes int16 por instrução.

## Decisão

Vamos armazenar cada dimensão como **int16 com escala 10000**: [0,1] → [0,10000], e o sentinela de `last_transaction` nula (−1) → −10000 (`I16_SCALE` / `I16_SENTINEL` em `c-impl/src/compat.h`). Cada registro ocupa 16 lanes (14 dims + 2 de padding zero) = 32 bytes, um registrador ymm exato. A escala foi primeiro 5000 (`31eed90`, "lossless detection") e depois refinada para 10000 (`c4814a6`) para cortar flips de fronteira que alimentavam a `absolute_penalty` da pontuação.

## Consequências

- A resolução de quantização passa de 256 para 10001 níveis por dimensão — o erro de quantização deixa de ser o fator limitante da detecção.
- Memória do dataset dobra em relação a uint8 (32 bytes/registro contra 16), ainda confortável no orçamento: o índice completo tem ~118 MB, servido por `mmap` compartilhado entre as 2 instâncias.
- A análise de overflow fica documentada em `compat.h`: pior `dist_sq` = 2·(20000²) + 12·(10000²) ≈ 2,0e9 < `INT32_MAX`, com as lanes do `vpmaddwd` seguras porque as dimensões de sentinela (5 e 6) caem em pares separados.
- Qualquer mudança de escala exige regerar o índice e recompilar (constante compartilhada entre builder Go e runtime C via formato v14).

## Alternativas consideradas

- **Manter uint8** — rejeitada: 3× menos memória, mas o erro de quantização era o piso da detecção.
- **float32** — rejeitada: dobraria a memória de novo e abandonaria o caminho inteiro `vpmaddwd`, reduzindo o throughput dos kernels de distância.
