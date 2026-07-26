# 0001 — Busca 5-NN exata em vez de índice aproximado (IVF/HNSW)

- **Status**: Aceita
- **Data**: 2026-05-17 (aprox., commit `99fc131`)

> ADR retroativo, reconstruído a partir do código em 2026-07-25.

## Contexto

O avaliador do desafio rotula seus payloads usando 5-NN exato com distância Euclidiana (`docs/spec/VECTOR_SEARCH.md`), e a especificação permite explicitamente força bruta. A primeira abordagem do repositório usava índices aproximados: um suposto HNSW (que a auditoria `AUDIT_FINDINGS.md` constatou não construir um grafo HNSW de fato) e depois IVF com probe ajustável (`TUNING.md` ainda documenta esses parâmetros). Todo índice aproximado introduz risco de recall — vizinhos verdadeiros fora do resultado — que se paga em pontos de detecção.

## Decisão

Vamos calcular o 5-NN **exato** sobre os 3.000.000 de vetores de referência, sem probe, sem trade-off de recall. A decisão está registrada como D2 em `IMPLEMENTATION_PLAN.md` e no commit `99fc131` ("switch to brute-force exact k-NN, drop IVF").

## Consequências

- A pontuação de detecção fica máxima **por construção**: o resultado coincide com o gabarito do avaliador (a menos do erro de quantização — ver ADR 0004). Medição de recall e tuning de probe deixam de existir.
- Todo o risco migra para a coluna de latência: varrer 3M de vetores por query custa dezenas de ms. As respostas a esse custo são as escalações previstas no próprio plano: kernels AVX2, particionamento e, por fim, a KD-tree exata (ADR 0003) — que mantém a exatidão e elimina o custo.
- Memória cai para o dataset puro + flags (~dezenas de MB), sem estruturas de grafo.

## Alternativas consideradas

- **HNSW** — rejeitada: complexidade alta de construção/validação e risco de recall; a implementação existente nem construía o grafo corretamente (achado C6 da auditoria).
- **IVF com probe** — rejeitada como caminho principal: recall ~99% custa pontos de detecção; mantida no plano apenas como último recurso de latência, e nunca precisou ser usada.
