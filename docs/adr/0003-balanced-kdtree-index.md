# 0003 — KD-tree balanceada como índice de busca exata

- **Status**: Aceita
- **Data**: 2026-05-31 (aprox., commit `5e9db73`, Phase 1)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

Mantendo o compromisso de exatidão (ADR 0001), a latência dependia de quantos registros são efetivamente escaneados por query. A linha evolutiva de particionamento por feature-hash (commits `2637a96` → `921935d`: 256 → 16384 partições, com probe por distância de Hamming e bound-pruning) chegou a ~76.000 registros escaneados por query — exato, mas caro, e com estrutura de partição cada vez mais entrelaçada com os bits do hash.

## Decisão

Vamos indexar os vetores com uma **KD-tree balanceada e exata** (`tools/build_kdtree.go` → formato descrito em `docs/ARCHITECTURE.md`): split sliding-midpoint na dimensão de maior spread, folhas de 32 registros, e a bounding box (AABB) de cada subárvore armazenada no próprio nó, para que o runtime faça a poda da descida com o kernel AVX2 `bound_dist_sq`. Os vetores são reordenados na ordem das folhas para varredura sequencial.

## Consequências

- Registros escaneados por query caem de ~76.000 para **~244** (~40× menos que a partição anterior; medida no comentário do `Makefile` da raiz) — mantendo recall exata por construção, sem parâmetro de tuning.
- O arquivo de índice cresce com os nós (72 bytes/nó), mas continua imutável e `mmap`-ável (~118 MB no total).
- A poda depende de aritmética barata sobre i16 — o que casa com a decisão de quantização do ADR 0004.
- A exatidão da árvore é verificada por teste dedicado (`c-impl/tests/test_kd_exact.c`) comparando contra brute force.

## Alternativas consideradas

- **Partição por feature-hash com bound-pruning** (linha v0.6–v0.12) — rejeitada: exata, porém escaneia ~300× mais registros; o builder `tools/build_partition_hash.go` permanece no repositório como referência.
- **Força bruta pura sobre os 3M** — rejeitada: ~15–30 ms de CPU por query no hardware-alvo (estimativa do `IMPLEMENTATION_PLAN.md`), incompatível com p99 ≤ 1 ms.
- **Índices aproximados** — já rejeitados no ADR 0001.
