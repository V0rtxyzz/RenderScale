# RenderScale Bedrock (port + diagnóstico)

Mod nativo `.so` para o **LeviLauncher** que muda a resolução de renderização do 3D
**sem afetar a UI**, inspirado no mod Java RenderScale. Slider de 30% a 100% no Mod Menu.

> **Status: não testado em aparelho.** Compila-se no GitHub Actions. A assinatura foi
> verificada contra o `libminecraftpe.so` 1.26.52 (1 match único), mas o efeito visual
> ainda precisa ser confirmado.

## Como funciona

O renderer `dragon` do Bedrock já tem um controlador de resolução dinâmica
(`mce::framebuilder::DynamicResolutionController`) que, todo frame, calcula a escala do 3D e
a entrega ao passe TAAU. O mod hooka o `update` desse controlador e, depois do original,
grava a escala do slider:

| Campo (versão 26.52)        | Significado                                   |
|-----------------------------|-----------------------------------------------|
| `obj + 0x00` (double)       | escala atual, 1.0 = 100%                      |
| `obj + 0x10` (u8)           | se `== 1`, o jogo lê a escala em `obj + 0x90` |

O hook é instalado por assinatura (`kSigDrcUpdate` em `src/main.cpp`), esperando o
`libminecraftpe.so` carregar (hook em `dlopen`, igual ao BedrockTools).

## Requisitos para ter efeito

- Minecraft Bedrock **1.26.52** (outras versões: refazer a assinatura, ver abaixo).
- **Upscaling ligado** em Configurações > Vídeo (pipeline deferred / Vibrant Visuals).
  Sem isso o jogo nem executa o controlador.

## Build (GitHub Actions)

1. Crie um repositório no GitHub e suba esta pasta.
2. Aba **Actions** > workflow **Build** (roda sozinho no push; ou "Run workflow").
3. Baixe o artefato **RenderScaleBedrock-levipack** e importe o `RenderScaleBedrock.levipack` no Levi.

Local (se tiver NDK): `xmake f -p android -a arm64-v8a -m release --ndk=<caminho>` e `xmake`.

## Log (para saber se está funcionando)

O mod escreve `renderscale.log` **na mesma pasta do `config.json`** do mod (abra pelo
gerenciador de arquivos; o caminho exato aparece na linha `config:` do log). Também envia
tudo para o logcat (`logcat -s RenderScale`). O arquivo é recriado se passar de 512 KB.

Linhas e o que significam:

| Linha | Significado |
|-------|-------------|
| `RenderScale 0.1.0 carregado` | o mod foi carregado pelo Levi |
| `assinatura encontrada ... offset 0x10fcdfec` | achou a função certa (offset esperado na 1.26.52) |
| `assinatura ... nao encontrada` | versão do Minecraft diferente: refazer a assinatura |
| `hook instalado` | hook em vigor; falta o jogo chamar a função |
| `hook ATIVO: update chamado pela 1a vez (... valor_do_jogo=X)` | **funcionando**: o jogo chamou o controlador. `X` deve ser um número de 0.3 a 1.0; se for algo estranho, o campo está errado |
| `frames=600 \| jogo calculou min.. max.. \| aplicado=..` | a cada 600 chamadas: o que o jogo calculou e o que o mod aplicou |
| `campo inesperado ... NAO vou escrever` | trava de segurança: valor fora do esperado, o mod não grava (evita corromper memória) |
| `escala alterada para ..` / `modulo LIGADO/DESLIGADO` | ações no Mod Menu |

Diagnóstico rápido:
- Tem `hook instalado` mas nunca `hook ATIVO`: o Upscaling está desligado (ou o pipeline
  não é o deferred). Ligue em Configurações > Vídeo.
- Tem `hook ATIVO` e `aplicado=` muda com o slider, mas a imagem não: me mande o log.

## Se não funcionar

- *"assinatura ... não encontrada"*: versão diferente. Gere nova com `mksig.py`
  (skill bedrock-pe-modding) no endereço do `update` e troque `kSigDrcUpdate`.
- Hook ativo mas sem mudança visual: o campo pode ser outro nessa versão (ver `UpscalingData`,
  getter de escala em `+0x04`). Envie o `renderscale.log`.

## Versão 0.2.0-diag: sem Vibrant Visuals

O hook do controlador de resolução dinâmica só roda com Vibrant Visuals. Sem ele, o log mostra
`update chamado 0 vezes`. Para o renderer OpenGL ES comum, esta versão traz um **diagnóstico**:
intercepta (pela GOT do `libminecraftpe.so`) as chamadas GL principais e grava a linha do
tempo de 2 frames. Serve para achar onde o 3D termina e o HUD começa, que é o que falta para
escalar só o 3D e manter o HUD nítido.

Como capturar:
1. Instale o `RenderScaleBedrock.levipack` (o nome da lib é diferente do RenderScale do
   NexCaise, então os dois podem coexistir; deixe o do NexCaise **desligado** na captura).
2. Entre em um mundo, com o HUD visível.
3. No Mod Menu, mude o interruptor **"Capturar frames GL"** (qualquer mudança arma a captura).
4. Pegue `gltrace-1.txt` na mesma pasta do `renderscale.log` e envie.

Formato: um evento por linha (`BIND`, `VIEWPORT`, `CLEAR`, `ENABLE/DISABLE`, `DRAW xN ...`) e um
`## RESUMO` por frame (draws com depth ligado/desligado, framebuffers usados, viewports).

## Créditos e licença

Ideia do mod Java RenderScale (fork do ResolutionControl++). O padrão de hook/menu segue o
BedrockTools (GPL v3) e o SDK `pl::` do LeviLauncher; escolha a licença do seu repositório
levando isso em conta. Este projeto **não** contém código do RenderScale de NexCaise.
