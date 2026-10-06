# Apex Radiance — guia técnico de Lot Streaming

**Objetivo:** registrar como o trabalho de Lot LOD/streaming foi pesquisado, validado e integrado ao Apex Radiance, de forma que possa ser reaplicado ou portado futuramente sem depender do histórico da conversa.

**Snapshot do código Apex auditado:** `e2a7f6944650c79a78165434a12fd27f142409a7` (main após integração da release final upstream 2.6.0).  
**Upstream Apex final integrado:** `loinyx/Sims3-ApexRadiance` tag `v2.6.0`, commit `9ca0b102d4ee0f90f5f4fe406d97ab0f3f5dca9a`.  
**S3SS comparado:** `sims3fiend/Sims3SettingsSetter` main `5eb2c65bb11e21dac423731c9726627f1fb118ac`; arquivo `patches/lot_streaming_optimizations_patch.cpp` blob `def668c514230dd83855c4b2c7e7af6e6e3b7add`.  
**Jogo usado nos probes controlados:** EA App 1.69.47.024017.

> Este documento separa explicitamente três origens: código nativo da EA, comportamento/código com linhagem do S3SS e pesquisa/implementação própria do Apex.

---

## 1. Estado final que deve ser preservado

A integração atual usa a **release final 2.6.0 do Apex upstream** e mantém as extensões do nosso fork que não existem nela:

- **Extended Lot Detail**: distância validada 70–300, valor recomendado/testado 300; capacidade 8–16, valor recomendado/testado 16.
- **Smooth Lot Streaming**: usa o throttle nativo do jogo e `WorldManager+0xEC = 5.0`.
- **Spread Lot Objects While Loading**: port/adaptação do Object Throttle do S3SS, com integração Apex.
- **Keep Lot Visibility Stable**: mesmo patch comportamental do S3SS (JZ -> JMP), com ownership/restauração próprios.
- **Pause Lot Streaming in Map View**: mesma finalidade do S3SS, mas implementação diferente no Apex.
- **Spread New Objects Over Frames / SceneNodeBudget**: recurso próprio do Apex, posterior no pipeline; não é port do S3SS.
- Ajustes de **bloom** do nosso fork permanecem separados do Lot Streaming.

### Estado padrão atual

Os valores **300 / 16 / 5.0** são o baseline validado/recomendado quando os recursos são ativados.  
Porém, no código final integrado à v2.6.0:

- `Extended Lot Detail`: `enabledByDefault = false`;
- `Smooth Lot Streaming`: `enabledByDefault = false`;
- Map View blocker, Object Throttle e Visibility Override: também opt-in.

Não confundir “valor default do controle” com “patch habilitado automaticamente”.

---

## 2. Pipeline: onde cada peça atua

A ordem conceitual é:

1. **EA decide candidatos a Detailed View** usando a métrica nativa de Lot LOD.
2. **Extended Lot Detail** altera os limites nativos do `WorldManager`:
   - `+0xDC`: distância;
   - `+0xE4`: número máximo de lotes detalhados.
3. **Smooth Lot Streaming** altera a política nativa de transições:
   - throttle nativo ligado;
   - `WorldManager+0xEC = 5.0`.
4. **Visibility Override** opcional remove o viés de ângulo da câmera na métrica.
5. **Map View Blocker** opcional usa `WorldManager+0x258` para pausar streaming durante o mapa.
6. Quando um lote já foi promovido, **Lot Object Throttle** divide `Lot::AddLotObjectsToScene` em janelas.
7. Mais tarde, no pipeline de cena, **SceneNodeBudget** limita quantos scene nodes pendentes são drenados por frame.

Por isso **Lot Object Throttle** e **SceneNodeBudget** não são duplicatas: atuam em etapas diferentes.

---

## 3. Extended Lot Detail — pesquisa Apex, não S3SS

### 3.1 Campos nativos identificados

Arquivo: `features/lot_detail_range.cpp/.h`.

No `WorldManager` vivo:

- `WorldManager + 0xDC` = **Lot LOD distance**;
- `WorldManager + 0xE4` = **Max Active Lots**.

O probe controlado demonstrou:

- distância 200 -> cutoff observado próximo de `200² = 40.000`;
- distância 300 -> cutoff observado próximo de `300² = 90.000`;
- com `+0xE4 = 8`, a área densa saturava em 8 lotes Detailed View;
- mudando somente `+0xE4` para 16, a mesma área chegou a 16 lotes Detailed View.

Isso é pesquisa do Apex. Não veio do patch público de Lot Streaming do S3SS comparado.

### 3.2 Faixas mantidas

```cpp
inline constexpr int kDefaultDistance = 300;
inline constexpr int kDefaultMaxActiveLots = 16;
inline constexpr int kMinDistance = 70;
inline constexpr int kMaxDistance = 300;
inline constexpr int kDistanceStep = 10;
inline constexpr int kMinActiveLots = 8;
inline constexpr int kMaxActiveLots = 16;
```

### 3.3 Regra de ownership

Não escrever cegamente nesses offsets. O Apex:

1. captura os valores do `WorldManager` atual;
2. faz write com valor esperado;
3. se o jogo recolocar exatamente o baseline capturado enquanto o mundo estabiliza, reaplica;
4. se aparecer um terceiro valor, considera que outro dono modificou o campo e **abandona a manutenção**;
5. no Stop, restaura somente se o valor atual ainda for exatamente o valor que o Apex aplicou;
6. nunca restaura por ponteiro de um `WorldManager` antigo.

Trecho central atual:

```cpp
if (!WriteExpected(g_worldManager + 0xDC, g_targetDistance, current))
    return false;

if (g_originalDistanceValid && current == g_originalDistance) {
    WriteExpected(g_worldManager + 0xDC, g_targetDistance, current);
} else if (current != g_targetDistance) {
    g_distanceAbandoned = true;
}
```

A mesma regra é usada para `+0xE4`.

### 3.4 Como reaplicar futuramente

Arquivos necessários:

- `features/lot_detail_range.cpp`
- `features/lot_detail_range.h`
- `patches/performance_patches.cpp`
- `patches/performance.h`
- `framework/game_addresses.*` para `WorldManagerPtr`
- `ApexRadiance.vcxproj`

No registro do patch, manter a distinção:

- distância/capacidade = **Apex**;
- threshold interno 12 = outra configuração, não é `Max Active Lots`.

---

## 4. Smooth Lot Streaming — comportamento com linhagem S3SS, wrapper Apex

Arquivo: `features/lot_lod_streaming.cpp/.h`.

O S3SS público usa três live settings na subfeature `streamingSettings`:

- `Throttle Lot LoD Transitions = true`;
- `Throttle Lot LoD Transitions Max Active Lot Threshold = 12`;
- `Camera speed threshold = 5.0`.

No nosso baseline de produção, o recurso principal Smooth Lot Streaming usa:

- throttle nativo ligado;
- `WorldManager+0xEC = 5.0`.

O controle de threshold 12 foi mantido apenas como recurso de desenvolvimento/referência separado; não é a capacidade 16 e não deve voltar a ser apresentado como “Maximum detailed lots”.

### 4.1 Implementação atual

O Apex resolve o byte nativo do throttle via `GameAddr::LotLodThrottleFlag`, e o `WorldManager` via `GameAddr::WorldManagerPtr`.

```cpp
g_throttleFlag = GameAddr::Get(GameAddr::Id::LotLodThrottleFlag);
g_worldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
```

Para cada mundo:

```cpp
float current = 0.0f;
Read(world + 0xEC, current);

if (current != kCameraThreshold)
    WriteExpected(world + 0xEC, kCameraThreshold, current);
```

onde:

```cpp
inline constexpr float kCameraThreshold = 5.0f;
```

### 4.2 Coexistência com S3SS

Antes de escrever, o Apex verifica se o S3SS oficial está carregado e se:

`LotStreamingOptimizations.streamingSettings = true`.

Se estiver, o Apex entra em modo “handled by S3SS” e não escreve esses valores.

Não basta existir um `S3SS.toml` antigo no disco: a DLL/ASI precisa estar carregada no processo.

### 4.3 Diferença para S3SS

O S3SS usa sua infraestrutura de `LiveSetting::Patch` + maintained writes.  
O Apex não copiou essa infraestrutura: usa `GameAddr`, `MemPatch`, `Tick()`, baseline por `WorldManager` e regra própria de ownership/restauração.

**Classificação:** comportamento/default com linhagem do S3SS; integração e safety wrapper do Apex.

---

## 5. Threshold 12 — não confundir com capacidade 16

Arquivo: `features/lot_active_threshold.cpp`.

O nome nativo é:

`Throttle Lot LoD Transitions Max Active Lot Threshold`.

O S3SS fixa esse valor em 12. O Apex reproduziu isso em um módulo separado para diagnóstico/referência.

No nosso teste sem S3SS carregado, esse valor **já estava em 12 antes de o Apex escrever qualquer coisa**. Portanto:

- não é evidência de que o S3SS estava ativo;
- não é `WorldManager+0xE4`;
- não é “Maximum detailed lots”;
- não é necessário para provar 300/16.

O recurso continua no código, mas a UI principal não deve usá-lo como controle normal do baseline 300/16.

---

## 6. Keep Lot Visibility Stable — mesmo patch comportamental, ownership Apex

Arquivo: `features/lot_visibility_override.cpp`.

O S3SS localiza um short conditional branch da métrica de visibilidade e altera:

`0x74 (JZ) -> 0xEB (JMP)`.

O Apex faz a mesma mudança comportamental, mas com regras adicionais:

- se o S3SS oficial estiver carregado e dono da subfeature, Apex não escreve;
- só aceita estado inicial `0x74` ou `0xEB`;
- se já for `0xEB`, considera dono externo;
- só restaura `0x74` se foi o Apex quem escreveu o `0xEB`;
- se outro mod trocar o opcode depois, Apex não sobrescreve na restauração.

Trecho atual:

```cpp
if (current == 0x74) {
    WriteExpectedByte(g_address, 0xEB, 0x74);
    g_owned = true;
} else if (current == 0xEB) {
    g_externalPatched = true;
}
```

**Classificação:** patch/ideia com linhagem direta do S3SS; integração e ownership próprios do Apex.

---

## 7. Pause Lot Streaming in Map View — conceito S3SS, implementação diferente

O S3SS:

- detoura `WorldManager::Update`;
- consulta `Camera_IsMapViewModeEnabled`;
- durante map view (e 1 s de grace), altera temporariamente `WorldManager+0x258`;
- chama o original e restaura o byte ao redor daquela chamada.

O Apex **não usa esse detour**.

No Apex:

- `MapView::IsOpen()` já fornece o estado;
- o pump normal do patch mantém `WorldManager+0x258 = 1` enquanto o mapa está aberto + 1000 ms;
- quando termina, o valor anterior é restaurado com ownership seguro;
- se o S3SS for o dono, Apex não escreve.

Trecho conceitual:

```cpp
const bool shouldBlock = inMapView || grace;

if (shouldBlock)
    MaintainMapBlock(&error);
else if (g_mapOriginalValid)
    RestoreMapBlockIfOwned();
```

**Classificação:** finalidade e gate nativo com linhagem S3SS; implementação standalone diferente.

---

## 8. Spread Lot Objects While Loading — port/adaptação direta do S3SS

Arquivos:

- `features/lot_object_throttle.cpp/.h`
- `framework/game_addresses.cpp/.h`
- `framework/entry_chain.*`
- registro em `patches/performance_patches.cpp`

Este é o ponto em que existe **linhagem direta de código/algoritmo**, não apenas uma ideia semelhante.

O próprio `lot_object_throttle.h` registra isso:

`Per-lot object streaming throttle, ported from Sims3SettingsSetter's LotStreamingOptimizations objectThrottle.`

E `game_addresses.cpp` congela explicitamente a referência do S3SS em `5eb2c65`.

### 8.1 Engine functions usadas

O port resolve:

- `Lot::AddLotObjectsToScene`;
- `Lot::UpdateObjectSceneNode`;
- ctor/dtor de `ScriptMessageScope`;
- `PostRemoteMethodCall`;
- `IsObjectLargeOrFlora`.

Os signatures de EA 1.69 foram trazidos/adaptados do trabalho do S3SS e integrados ao `GameAddr` do Apex.

### 8.2 Algoritmo preservado

- default: **2 objetos regulares por janela**;
- delay: **16 ms**;
- shells/prédios/geometria exterior/flora grandes são construídos imediatamente na primeira janela;
- objetos regulares são divididos em janelas;
- a continuação é repostada pelo mecanismo nativo `PostRemoteMethodCall`;
- se o lote mudar de estado Detailed View no meio do processo, a continuação é cancelada;
- o início do vetor de objetos é guardado para não retomar um estado pertencente a uma lista antiga/reutilizada.

Trecho atual do Apex:

```cpp
if (idx == 0) {
    for (size_t i = 0; i < count; ++i) {
        if (begin[i] && g_isObjectLargeOrFlora(begin[i]) != 0)
            g_updateObjectSceneNode(lot, begin[i], initialLoad, alwaysVisibleOnly);
    }
}

const int quota = std::clamp(g_objectsPerWindow.load(), 1, 256);
for (; pos < count && built < quota; ++pos) {
    if (begin[pos] && g_isObjectLargeOrFlora(begin[pos]) != 0) continue;
    g_updateObjectSceneNode(lot, begin[pos], initialLoad, alwaysVisibleOnly);
    ++built;
}
```

### 8.3 O que o Apex mudou em relação ao S3SS

O algoritmo central foi portado, mas a integração foi adaptada:

- hook através de **`EntryChain`**, não `PatchHelper::WriteRelativeJump` direto;
- resolução através de **`GameAddr`**;
- detecção de S3SS e recusa de dupla instalação;
- contadores/StatusText próprios;
- lifecycle controlado pelo framework `ApexPatch`;
- ranges/settings passam pelo sistema de configuração do Apex;
- rollback pelo `EntryChain::Remove`;
- a feature é separada de SceneNodeBudget.

Se for portar novamente, não copiar o framework do S3SS: portar o algoritmo para o framework de destino, preservando as condições de segurança acima.

---

## 9. Spread New Objects Over Frames — NÃO veio do S3SS Object Throttle

Arquivos: `features/scene_budget.*`.

Este recurso atua depois, no drain de scene nodes em `Scene::BeginFrame`.

O S3SS Object Throttle limita **criação/adição de objetos do lote**.  
SceneNodeBudget limita **processamento de nodes já enfileirados**.

Os dois podem coexistir.

O próprio documento `docs/features/performance/scene-node-budget.md` registra que o S3SS detoura `AddLotObjectsToScene`, enquanto SceneNodeBudget trabalha nos sites `SceneDrain`, `SceneNodeDtor`, `SceneAddNode` e `SceneHolderTeardown`.

**Classificação:** pesquisa/implementação Apex, não port do S3SS LotStreamingOptimizations.

---

## 10. Matriz de proveniência S3SS x Apex

| Área | Origem real | O que usamos no Apex |
|---|---|---|
| Métrica/estruturas Lot LOD do jogo | EA | Ambos os mods trabalham sobre o mesmo engine |
| `WorldManager+0xDC` distância 300 | Pesquisa Apex | Exclusivo do nosso trabalho validado |
| `WorldManager+0xE4` capacidade 16 | Pesquisa Apex | Exclusivo do nosso trabalho validado |
| Probe 200² / 300² | Pesquisa Apex | Exclusivo |
| Max Active Lots 8 -> 16 A/B | Pesquisa Apex | Exclusivo |
| Throttle Lot LoD Transitions | EA, comportamento exposto pelo S3SS | Mesmo comportamento; wrapper Apex |
| Camera speed threshold 5.0 | Valor/default usado pelo S3SS | Mesmo valor, validado no A/B Apex |
| Threshold interno 12 | S3SS | Módulo separado de referência; não é capacidade 16 |
| Visibility JZ -> JMP | S3SS | Mesmo byte patch, safety/ownership Apex |
| Map-view blocker | S3SS | Mesmo objetivo/gate, implementação Apex sem detour de WorldManager::Update |
| Object Throttle | S3SS | **Port/adaptação direta** do algoritmo |
| SceneNodeBudget | Apex | Não veio do S3SS |
| Ownership de +0xDC/+0xE4 | Apex | Não veio do S3SS |
| Metric Probe / logs controlados | Apex | Não veio do S3SS |
| Bloom de paredes/cinema | Apex fork | Fora do Lot Streaming/S3SS |

---

## 11. Mapa função por função: S3SS -> Apex

Esta tabela é o atalho para manutenção futura. Ela compara a implementação pública do S3SS auditada com o código atual do Apex.

| S3SS | Apex | Relação |
|---|---|---|
| `Detour_AddLotObjectsToScene` | `Hook_AddLotObjectsToScene` em `features/lot_object_throttle.cpp` | **Port/adaptação direta do algoritmo.** Mantém estado por lote, quota de objetos regulares, shells/flora síncronos e cancelamento quando Detailed View muda. |
| `DrainPending` | `LotObjectThrottle::Tick()` | **Port/adaptação direta.** Aguarda o delay e usa `PostRemoteMethodCall` para a continuação. |
| `InstallThrottle` + `PatchHelper::WriteRelativeJump` | `LotObjectThrottle::Start()` + `EntryChain::Install` | Mesmo ponto funcional; infraestrutura de hook foi reescrita para o framework Apex. |
| `Uninstall` + `PatchHelper::RestoreAll` | `LotObjectThrottle::Stop()` + `EntryChain::Remove` | Mesmo lifecycle; rollback implementado pelo framework Apex. |
| `InstallVisibility` | `LotVisibilityOverride::Start()` | Mesmo patch `0x74 -> 0xEB`; Apex adiciona detecção de owner externo e rollback condicional. |
| `Hooked_WorldManagerUpdate` | **não portado** | Apex não detoura `WorldManager::Update` para map view. |
| map-view write em `WorldManager+0x258` | `LotLodStreaming::TickMapViewBlocker()` | Mesmo gate nativo e mesma janela de grace de 1000 ms, mas implementação própria via pump. |
| `TryApplyStreamingSettings` — throttle | `LotLodStreaming::Start()/Tick()` | Mesmo comportamento nativo; Apex resolve o byte e mantém/restaura com ownership próprio. |
| `TryApplyStreamingSettings` — camera threshold 5 | `WorldManager+0xEC` em `lot_lod_streaming.cpp` | Mesmo valor de referência, validado também pelos A/B do Apex. |
| `TryApplyStreamingSettings` — threshold 12 | `LotActiveThreshold::Start()/Tick()` | Mantido separado para desenvolvimento/referência; não é a capacidade 16. |
| S3SS patch framework | `ApexPatch`, `GameAddr`, `MemPatch`, `EntryChain` | **Não portado.** Apex usa sua própria infraestrutura. |
| S3SS UI/config | Performance UI + Apex TOML | **Não portado.** Apenas os comportamentos selecionados foram integrados. |

### 11.1 Nomes/constantes do Object Throttle que foram preservados conceitualmente

A implementação Apex mantém os mesmos dados essenciais necessários para o algoritmo:

- offsets do vetor de objetos do lote: `+0x14` / `+0x18`;
- `DetailedViewRequested`: `+0xC1`;
- `Bulldozing`: `+0xC9`;
- ScriptMessageScope begin/end: `0x04C55E8C` / `0x04C55EFA`;
- default de **2 objetos regulares por janela**;
- default de **16 ms** entre janelas;
- `PostRemoteMethodCall(thread=1, ...)` para repostar a continuação;
- `IsObjectLargeOrFlora` para excluir shells/flora do throttle.

No Apex esses valores estão em `features/lot_object_throttle.cpp`; não devem ser duplicados em outra feature.

### 11.2 Signatures: onde manter

Os signatures usados pelo port ficam centralizados em `framework/game_addresses.cpp`, grupo `LotObjectThrottle`, e não dentro do feature.

O comentário atual registra a origem de auditoria:

`Per-lot object streaming throttle (S3SS LotStreamingOptimizations objectThrottle, frozen 5eb2c65)`.

Se o executável alvo mudar:

1. validar novamente cada signature contra a função real;
2. atualizar `GameAddr`, não espalhar endereços fixos no feature;
3. exigir que o grupo inteiro resolva antes de instalar;
4. em caso de ambiguidade, falhar fechado.

### 11.3 O que foi deliberadamente melhorado no port Apex

Além da troca de framework, o Apex acrescenta:

- detecção de S3SS por subfeature antes de instalar;
- `EntryChain` para coexistência com outros layers no mesmo entry point;
- contadores de chamadas, janelas, objetos regulares, shells/flora, posts e cancelamentos;
- `StatusText()` para diagnóstico;
- clamps explícitos das configurações;
- separação formal entre Lot Object Throttle e SceneNodeBudget;
- endereço/resolução centralizados em `GameAddr`;
- política de restore/owner consistente com o restante do Apex.

---

## 12. O que NÃO copiamos do S3SS

Do `LotStreamingOptimizations` público comparado, o Apex **não carrega wholesale**:

- classe/framework `OptimizationPatch`;
- `PatchHelper` como sistema de patches;
- `DetourHelper` como infraestrutura geral;
- sistema `LiveSetting::Patch`/maintained writes;
- o detour de `WorldManager::Update` usado pelo map blocker;
- UI/configuração do S3SS;
- arquivos de framework do S3SS;
- o threshold 12 como substituto da nossa capacidade 16;
- qualquer lógica para distância 300 ou `WorldManager+0xE4 = 16`, porque essas partes não existem naquele patch S3SS comparado.

O Apex usa seus próprios:

- `GameAddr`;
- `MemPatch`;
- `EntryChain`;
- `ApexPatch`;
- `S3SSDetect`;
- pump/`Tick()`;
- regras de ownership e rollback.

---

## 13. Detecção e cooperação com S3SS

Arquivo: `framework/s3ss_detect.cpp/.h`.

A regra é **cooperar, não disputar o mesmo endereço**.

Quando o S3SS oficial está carregado e a subfeature correspondente está ativada:

- Smooth Lot Streaming -> Apex não escreve streaming settings;
- Map View Blocker -> Apex não toca no skip gate;
- Object Throttle -> Apex não instala o EntryChain layer;
- Visibility Override -> Apex não troca o opcode;
- Threshold 12 -> Apex não escreve.

Isso deve continuar em qualquer port futuro.

---

## 14. Registro no framework Apex

O wiring atual está em `patches/performance_patches.cpp`.

Classes:

- `LotDetailRangePatch`
- `LotLodStreamingPatch`
- `MapViewStreamingBlockerPatch`
- `LotObjectThrottlePatch`
- `LotActiveThresholdPatch`
- `LotVisibilityOverridePatch`

Padrão de lifecycle:

```cpp
bool Install() override {
    std::string error;
    if (!Subsystem::Start(..., &error)) return Fail(error);
    isEnabled = true;
    return true;
}

void Update() override {
    Subsystem::Tick();
}

bool Uninstall() override {
    Subsystem::Stop();
    isEnabled = false;
    return true;
}
```

Na hora de portar para outra branch, não basta copiar os `.cpp`: registrar no projeto, no patch framework, nos endereços e na UI.

---

## 15. Endereços e resolvers importantes

Arquivo: `framework/game_addresses.cpp/.h`.

IDs relevantes:

- `WorldManagerPtr`
- `LotLodScoring`
- `LotDetailRequest`
- `LotLodThrottleTest`
- `LotLodThrottleFlag`
- `LotVisibilityCameraBiasJZ`
- `LotAddObjectsToScene`
- `LotUpdateObjectSceneNode`
- `ScriptMessageScopeCtor`
- `ScriptMessageScopeDtor`
- `PostRemoteMethodCall`
- `IsObjectLargeOrFlora`

Para EA 1.69, os signatures do Object Throttle foram congelados a partir da comparação com S3SS `5eb2c65`; o feature group recusa instalar se os endereços necessários não puderem ser provados.

Regra para futuro: **fail closed**. Não adivinhar endereço.

---

## 16. Sequência recomendada para portar para uma futura versão

1. Começar do **upstream final publicado**, não de branch experimental.
2. Confirmar se o upstream já implementou parte do nosso código.
3. Portar `lot_detail_range.*` e registrar no projeto.
4. Portar `lot_lod_streaming.*` e confirmar os `GameAddr`.
5. Portar `lot_visibility_override.*` se ainda desejado.
6. Portar `lot_object_throttle.*` apenas com os signatures validados para a build alvo.
7. Manter `S3SSDetect` antes de qualquer escrita/hook concorrente.
8. Registrar os patches em `performance_patches.cpp`.
9. Expor UI sem confundir:
   - Maximum detailed lots = `+0xE4`;
   - threshold 12 = live setting diferente.
10. Compilar x86 Release.
11. Testar sem S3SS.
12. Testar com S3SS e confirmar “Handled by Sims3SettingsSetter” sem double hook.
13. Fazer A/B de transições na mesma câmera/sessão.
14. Só depois promover para main.

---

## 17. Validação que definiu o baseline

Com distância 300 + capacidade 16 fixas:

**Tudo OFF**
- 268 transições em ~107,7 s;
- 149,3 transições/min;
- 87 reversões do mesmo lote <= 5 s;
- 48 reversões <= 2 s.

**Smooth Lot Streaming ON**
- 190 transições em ~114,9 s;
- 99,2 transições/min;
- 18 reversões <= 5 s;
- 3 reversões <= 2 s.

Redução aproximada:

- 34% nas transições/min;
- 79% nas reversões <= 5 s;
- 94% nas reversões <= 2 s.

Não houve crash/fatal/exception nos testes decisivos registrados.

---

## 18. Integração com Apex upstream 2.6.0

Em 06/10/2026 o fork foi atualizado para a **release final upstream 2.6.0**.

Regra usada:

- trazer o estado final publicado;
- não trazer a branch pós-release `feature/color-filters`;
- onde upstream e nosso fork tinham duas implementações intermediárias da mesma correção EA 1.69, preferir a implementação final upstream;
- preservar recursos exclusivos nossos, principalmente 300/16 e bloom.

O arquivo `features/level_light_share.cpp` foi deliberadamente restaurado para a implementação oficial 2.6.0 depois que uma mesclagem híbrida gerou conflito de símbolos. A build seguinte passou.

Workflow de validação:
- run `37414470615`;
- x86 Release: **success**.

---

## 19. Limite importante: parede/bloom não é Lot Streaming

Durante a investigação visual da fachada foram testadas soluções de seam envolvendo WallSolve, ExactSeam e samplers S2/S6. O último teste piorou a iluminação ao normalizar S6 de draws diferentes.

Esses experimentos foram **removidos**.

Não reintroduzir no port de Lot Streaming:

- WallSolve resolver experimental da investigação;
- FacadeT2/ExactSeam;
- 4-state S2/S6;
- WallSeamS6 normalization.

O que permanece é a correção separada e validada de **bloom alpha** em `features/lot_light_bridge.cpp`, documentada em `docs/features/night-lighting-changelog.md`.

---

## 20. Checklist de rollback

Se uma versão futura der problema:

### Extended Lot Detail
- desativar o patch;
- confirmar que `+0xDC/+0xE4` só são restaurados se Apex ainda for o dono;
- não escrever através de `WorldManager` stale.

### Smooth Lot Streaming
- desligar e restaurar throttle/camera threshold apenas se ainda forem os valores do Apex;
- se S3SS estiver carregado, deixar S3SS como dono.

### Visibility Override
- restaurar `0xEB -> 0x74` somente se Apex fez a escrita original.

### Map View
- restaurar `+0x258` somente no `WorldManager` vivo e apenas se ainda estiver em 1 por causa do Apex.

### Object Throttle
- remover a camada `EntryChain::Layer::LotObjectThrottle`;
- limpar estados pendentes;
- não manter continuations depois do stop.

---

## 21. Arquivos que o Luís deve consultar primeiro

1. `docs/features/lot-streaming-changelog.md` — pesquisa e A/B.
2. `docs/Lot_Streaming_Implementation_Guide.md` — este handoff.
3. `features/lot_detail_range.cpp/.h` — 300/16.
4. `features/lot_lod_streaming.cpp/.h` — throttle, threshold 5 e map blocker.
5. `features/lot_object_throttle.cpp/.h` — port do S3SS.
6. `features/lot_visibility_override.cpp/.h`.
7. `features/lot_active_threshold.cpp/.h` — threshold 12 separado.
8. `framework/game_addresses.cpp/.h` — resolvers/signatures.
9. `framework/s3ss_detect.cpp/.h` — cooperação.
10. `patches/performance_patches.cpp` — registro/configuração/UI.
11. `docs/features/performance/scene-node-budget.md` — estágio posterior, independente.
12. `docs/features/night-lighting-changelog.md` — bloom e iluminação, fora do Lot Streaming.

---

## 22. Regra curta de autoria/proveniência

Ao descrever publicamente:

- **EA**: engine, structs, fields e funções nativas.
- **S3SS**: fonte/linhagem direta do Object Throttle; linhagem de comportamento para visibility override, map blocker, throttle settings, threshold 12 e camera threshold 5.
- **Apex fork**: pesquisa 300/16, probes, A/B, ownership/restauração, integração standalone, detecção/coexistência e SceneNodeBudget.

Evitar dois extremos incorretos:

- “não usamos nada do S3SS” — falso;
- “nosso Lot Streaming é código do S3SS” — também falso.

A formulação correta é: **há componentes com linhagem S3SS, especialmente o Object Throttle, integrados a um conjunto maior de pesquisa, segurança e controles próprios do Apex.**
