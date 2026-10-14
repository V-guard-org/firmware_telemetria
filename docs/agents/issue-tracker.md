# Issue tracker: GitHub

As issues deste repositório ficam no GitHub Issues de `V-guard-org/firmware_telemetria`. Use a CLI `gh`.

## Wayfinding (`/wayfinder`)

- **Mapa:** [issue #1](https://github.com/V-guard-org/firmware_telemetria/issues/1), com a etiqueta `wayfinder:map`.
- **Tickets:** são sub-issues do mapa, com as etiquetas `wayfinder:research`, `wayfinder:task`, `wayfinder:grilling` ou `wayfinder:prototype`. A etiqueta `precisa:isabela` marca o que afeta o código da Isabela.
- **Bloqueios:** usam as dependências nativas do GitHub. Para criar uma: `gh api --method POST repos/V-guard-org/firmware_telemetria/issues/<n>/dependencies/blocked_by -F issue_id=<id numérico do bloqueador>`.
- **Assumir um ticket:** atribuir ao dev que conduz o mapa (`nascpedro`).
- **Resolver:** comentar a resposta, fechar a issue e acrescentar uma linha em "Decisions so far" no mapa.
- **Pesquisas:** cada uma fica num ramo `research/<nome>`, com o arquivo `docs/research/<nome>.md`.
