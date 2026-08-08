# MCP サーバー

D3D12LookDevPTWinUI には、実行中の renderer を VS Code、Codex、独自 JSON-RPC client などから参照・操作するための local MCP server が入っています。WinUI editor と同じ validation-oriented renderer-command layer を経由します。

English documentation: [MCP Server](mcp.md)

## MCP 経由の操作例

MCP 対応 client から camera、quality、material、denoise の変更を指示できます。
WinUI の MCP panel には session、承認待ち、recent local JSON-RPC request が表示
されます。実 token は screenshot や project file に入れないでください。

client 側では先に validation を通し、mutation tool を適用し、その後 `get_state` で renderer state が同じ値になったことを確認します。現在の build では、`tools/list` または `lookdevpt://actions/schema` が返す live schema と、この文書の例を正として扱ってください。

## 利用条件とセキュリティ

- Endpoint: `http://127.0.0.1:<port>/mcp`
- Default port: `8777`
- Bind address: `127.0.0.1` のみ
- Transport: Streamable HTTP 形式の JSON-RPC over `POST /mcp`
- 対応 protocol version: `2025-11-25`、`2025-06-18`
- 認証: `bearer_token`（default）または `none`。`bearer_token` では `Authorization: Bearer <token>` が必須
- Session: `initialize` の response header で `MCP-Session-Id` を返し、以後の request では同じ header が必須
- Server-Sent Events: 未実装。`GET /mcp` は `405 Method Not Allowed`
- HTTP request body の上限: 16 MiB
- HTTP/1.1 request body は `Content-Length` と `Transfer-Encoding: chunked` の両方に対応します。chunk extension と trailer は安全に消費し、両 framing header を同時指定した曖昧な request は拒否します。

認証 mode、Bearer token、その他の MCP 設定は以下に保存されます。

```text
%APPDATA%\D3D12LookDevPTWinUI\settings.json
```

この file は user-local です。token を `.lookdevpt.json`、README、screenshot、issue comment、commit 済み VS Code 設定に入れないでください。

`Origin` header は absent、`null`、`http://127.0.0.1:*`、`http://localhost:*` だけ許可します。それ以外は `403` になります。

## サーバーの起動

dockable な `MCP Server` panel から操作できます。

- `Start Server` / `Stop Server`
- `Port`
- `Request Timeout`
- `Access Mode`
- `Authentication`
- `Copy Token`
- `Regenerate Token`
- `Export mcp.json...`
- pending approvals と recent request log

`Export mcp.json...` は、現在の endpoint と protocol version を含む VS Code
互換 MCP 設定を書き出します。default では password 形式の `inputs` prompt を使い、
Bearer token は file に保存しません。自己完結した local 設定が必要な場合だけ
`Embed bearer token in exported mcp.json` を有効にしてください。この場合 token は
平文で保存されるため、source control の外で保管し、共有しないでください。
Request timeout と access mode は application-local な server 設定であり、VS Code
の `mcp.json` schema には含まれないため export 対象外です。`Authentication` が
`None` の場合、export file から `inputs` と `Authorization` header の両方を省略します。

server は default disabled です。command line から明示的に起動することもできます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe --mcp-server --mcp-port 8777 --mcp-auth bearer_token --mcp-token <token> --mcp-access confirm_mutations
```

Access mode:

- `read_only`: read tool は使えます。mutation tool は拒否されます。
- `confirm_mutations`: mutation tool は WinUI の `MCP` panel で Approve されるまで待ちます。
- `allow_mutations`: mutation tool を UI 承認なしで実行します。

Authentication mode:

- `bearer_token`: default。すべての request に設定済み Bearer token が必要です。
- `none`: `Authorization` header なしで接続できます。server は引き続き `127.0.0.1` 固定で、外部 interface には公開されません。認証を無効にする場合は `read_only` または `confirm_mutations` の利用を推奨します。

command line から認証なしで起動する例:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe --mcp-server --mcp-port 8777 --mcp-auth none --mcp-access confirm_mutations
```

mutation queue は renderer-thread の safe point で処理され、同時に保持できる request は 16 件までです。HTTP server thread から D3D12 / WinUI state を直接触りません。

`capture_viewport` は read operation ですが、GPU readback を行うため renderer thread の queue を経由します。`capture_debug_pack` は一時的に debug view を変更し、その関連 temporal history を無効化するため mutation access が必要です（`confirm_mutations` では承認も必要）。`restoreView` の default は `true` です。

## Snapshot の更新頻度と鮮度

MCP read は、HTTP thread から renderer / scene data を直接走査せず、mutex で保護した snapshot を使います。server 停止中は snapshot 生成処理も停止します。server 起動時に最初の snapshot を強制生成し、起動中は次の頻度で更新します。

- `state`: 約 33 ms ごと（30 Hz）。
- `stats` と `diagnostics`: 約 100 ms ごと（10 Hz）。
- `materials`、`project`、`scene/summary`、material variants、presets: scene / project / catalog revision が変わった場合のみ再生成。
- debug-view、render-mode、action-schema resource: 固定 metadata から on demand で生成。

mutation 直後の read は、通常 1 回分の refresh interval だけ renderer より遅れることがあります。値を検証する client は、renderer が frame を更新している状態で、その interval 後に `get_state` をもう一度読んでください。mutation、validation、capture は renderer command queue 上で直列化されますが、通常の immutable snapshot read は renderer state の mutation を block しません。

## VS Code 設定

VS Code の MCP server 設定は、workspace の `.vscode/mcp.json` または user profile の `mcp.json` に保存します。現在の VS Code MCP configuration reference では、HTTP server に `type`、`url`、`headers` を使い、secret には optional の `inputs` を使えます。

`.vscode/mcp.json` の例:

```json
{
  "inputs": [
    {
      "type": "promptString",
      "id": "lookdevpt-token",
      "description": "D3D12LookDevPTWinUI MCP bearer token",
      "password": true
    }
  ],
  "servers": {
    "d3d12LookDevPT": {
      "type": "http",
      "url": "http://127.0.0.1:8777/mcp",
      "headers": {
        "Authorization": "Bearer ${input:lookdevpt-token}",
        "MCP-Protocol-Version": "2025-11-25"
      }
    }
  }
}
```

`Authentication` が `None` の場合は token input と Authorization header を省略します。

```json
{
  "servers": {
    "d3d12LookDevPT": {
      "type": "http",
      "url": "http://127.0.0.1:8777/mcp",
      "headers": {
        "MCP-Protocol-Version": "2025-11-25"
      }
    }
  }
}
```

編集後は VS Code の `MCP: List Servers` から server entry を start / restart してください。D3D12LookDevPTWinUI を rebuild して tool list が変わった場合は `MCP: Reset Cached Tools` を実行してください。

注意:

- VS Code 側の server entry を start する前に、D3D12LookDevPTWinUI 本体と MCP server を起動しておきます。
- `bearer_token` 利用時に WinUI の MCP panel で token を regenerate した場合は、VS Code 側の MCP server entry を restart し、新しい token を入力します。
- この server は HTTP POST JSON-RPC 対応です。SSE-only の MCP client では利用できません。

## JSON-RPC の流れ

client は最初に `initialize` を呼び、返ってきた session id を保持し、その後 `notifications/initialized` を送ります。

PowerShell 例:

```powershell
$endpoint = "http://127.0.0.1:8777/mcp"
$token = "<token>"
$headers = @{
  "Authorization" = "Bearer $token"
  "MCP-Protocol-Version" = "2025-11-25"
}

$initBody = @{
  jsonrpc = "2.0"
  id = 1
  method = "initialize"
  params = @{
    protocolVersion = "2025-11-25"
    capabilities = @{}
    clientInfo = @{ name = "manual-client"; version = "1.0" }
  }
} | ConvertTo-Json -Depth 10 -Compress

$init = Invoke-WebRequest -Uri $endpoint -Method Post -Headers $headers -ContentType "application/json" -Body $initBody
$sessionId = [string]$init.Headers["MCP-Session-Id"][0]

$sessionHeaders = @{
  "Authorization" = "Bearer $token"
  "MCP-Protocol-Version" = "2025-11-25"
  "MCP-Session-Id" = $sessionId
}

$initialized = @{
  jsonrpc = "2.0"
  method = "notifications/initialized"
  params = @{}
} | ConvertTo-Json -Depth 10 -Compress

Invoke-WebRequest -Uri $endpoint -Method Post -Headers $sessionHeaders -ContentType "application/json" -Body $initialized
```

server が `Authentication: None` の場合は `$headers` と `$sessionHeaders` の両方から
`Authorization` を省略します。session header と protocol header は引き続き必要です。

read tool の呼び出し:

```powershell
$body = @{
  jsonrpc = "2.0"
  id = 2
  method = "tools/call"
  params = @{
    name = "lookdevpt.get_state"
    arguments = @{}
  }
} | ConvertTo-Json -Depth 10 -Compress

Invoke-WebRequest -Uri $endpoint -Method Post -Headers $sessionHeaders -ContentType "application/json" -Body $body
```

session の終了:

```powershell
Invoke-WebRequest -Uri $endpoint -Method Delete -Headers $sessionHeaders
```

## Tools

Read tools:

- `lookdevpt.get_stats`: adapter、DXR tier、resolution、集約 GPU timing、scene counts、history / resource-memory 状態、active secondary shading rate、denoiser state、MCP queue state を返します。
- `lookdevpt.get_state`: scene / project path、quality / ray-budget 設定、camera、lighting、path tracing、ReSTIR / RTXDI 状態、denoise、frame-history revision、view state を返します。
- `lookdevpt.list_materials`: material 名、使用数、編集可能な PBR factor、texture slot 状態を返します。
- `lookdevpt.list_debug_views`: debug view の id、label、key を返します。
- `lookdevpt.list_render_modes`: render mode の label と action value を返します。
- `lookdevpt.get_diagnostics`: scene / project / capture / MCP diagnostics を返します。
- `lookdevpt.capture_viewport`: 現在の final/debug viewport を PNG として取得し、inline `image/png` と `lookdevpt://captures/latest.png` を返します。

Validation / capture workflow tools:

- `lookdevpt.validate_action`: `{ "method": "...", "params": { ... } }` を受け取り、同じ action path を `validateOnly=true` で実行します。
- `lookdevpt.run_actions`: 最大 16 個の action-layer call を 1 request で validation / apply します。validation 失敗時は一切 mutation しません。apply は指定順ですが、後段の runtime operation が失敗した場合の rollback transaction ではありません。
- `lookdevpt.capture_debug_pack`: 最大 8 個の debug view を PNG capture し、それぞれの resource link を返します。capture 中に debug-view / history state を変更するため mutation として扱います。

Mutation tools:

- `lookdevpt.reset_accumulation`
- `lookdevpt.reset_denoise_history`
- `lookdevpt.reset_reservoirs`
- `lookdevpt.reset_camera_view`
- `lookdevpt.set_camera_speed`
- `lookdevpt.fit_camera_to_scene`
- `lookdevpt.set_display_resolution`
- `lookdevpt.load_project`
- `lookdevpt.save_project`
- `lookdevpt.save_project_as`
- `lookdevpt.set_scene`
- `lookdevpt.set_camera`
- `lookdevpt.set_material`
- `lookdevpt.set_material_texture`
- `lookdevpt.reset_material`
- `lookdevpt.save_material_variant`
- `lookdevpt.apply_material_variant`
- `lookdevpt.delete_material_variant`
- `lookdevpt.set_material_view`
- `lookdevpt.set_color_management`
- `lookdevpt.set_lighting`
- `lookdevpt.set_path_tracing`
- `lookdevpt.set_quality`
- `lookdevpt.set_restir`
- `lookdevpt.set_denoise`
- `lookdevpt.set_view`

tool result は主に `structuredContent` を使います。互換用に text summary も含めます。

## Resources

- `lookdevpt://state`: 現在の state JSON。
- `lookdevpt://stats`: 現在の stats JSON。
- `lookdevpt://diagnostics`: scene、project、capture、MCP diagnostics。
- `lookdevpt://materials`: material list JSON。
- `lookdevpt://materials/{index}`: 1 material の JSON object。
- `lookdevpt://materials/{index}/textures`: 1 material の source/current/override texture slot。
- `lookdevpt://material-variants`: 保存済み per-material variant snapshot。
- `lookdevpt://material-presets`: built-in / user material preset。
- `lookdevpt://debug-views`: debug view の id、label、key。
- `lookdevpt://render-modes`: render mode と `set_path_tracing.mode` の value。
- `lookdevpt://project`: 現在の project path と dirty flag。
- `lookdevpt://scene/summary`: scene counts、bounds、lights、asset paths。
- `lookdevpt://actions/schema`: action 名と JSON input schema。
- `lookdevpt://captures/index`: memory 上の capture history。
- `lookdevpt://captures/latest.png`: 最新の PNG capture。
- `lookdevpt://captures/{id}.png`: `capture_viewport` または `capture_debug_pack` の PNG。

Resource templates:

- `lookdevpt://captures/{id}.png`
- `lookdevpt://materials/{index}`
- `lookdevpt://materials/{index}/textures`

Prompts:

- `lookdevpt.inspect_scene`: state / stats / materials / diagnostics を読み、scene を要約します。
- `lookdevpt.tune_denoise`: validation を通して安定した denoise 設定を提案・適用します。
- `lookdevpt.setup_camera_shot`: scene bounds と state を使って camera shot を作ります。
- `lookdevpt.capture_debug_review`: debug pack を capture し、見える問題を要約します。

## State、Stats、Benchmark metric

安定化 / 性能 pipeline に関連する主な `get_state` field は次のとおりです。

- `quality`: 保存または request された `qualityProfile`、`restirBackend`、`secondaryShadingRate`、完全な `rayBudget`、`finalTaa`、`sharpenStrength`、`referenceSpp` の policy object。profile と availability に依存する実効結果は `finalTaaActive`、`restir.effective`、`denoise.activeBackend` に分けて公開します。
- `finalTaaActive`: 選択 profile と利用可能な pipeline の組み合わせで Final TAA が実際に動作しているか。
- `pathTracing.requestedSecondaryShadingRate`、`activeSecondaryRate`、`autoSecondaryHalfActive`: request した policy と現在の secondary rate。
- `restir.requestedBackend`、`effective`、`rtxdiStatus`: request した RTXDI path、build / runtime availability、DI / GI の active 状態、fallback reason。
- `frameState`: frame / sample counter、change mask、有効 history-domain mask、camera-cut flag、独立した scene / geometry / material / light / HDRI / backend / profile revision。

主な `get_stats` group は次のとおりです。

- `gpuTiming`: 最後に完了した aggregate pipeline、Path Trace、ReSTIR reuse、denoise、copy、UI timing、および validity / completion serial。
- `historyDomains`: 有効 history mask と最後の change mask。
- `resourceMemory`: frame / history の bytes / MiB、512 MiB budget、budget 判定、active allocation profile。
- `secondaryShading`: requested / effective rate、active ratio、automatic half-rate state、追加 sample quota、bounce penalty、over / under-budget counter。
- `denoiser` と `mcp`: effective backend / history 状態と server queue state。

より重い per-phase metric（ReSTIR candidate / temporal / spatial / shade / publish、denoise prepare / core / composite、Final TAA、quality counters、history publish、詳細 CPU stage、推定 ray budget、history / contribution diagnostics）は benchmark の CSV / JSON artifact に記録します。`get_stats` は意図的に低コストな aggregate snapshot のままです。`performance` benchmark では full-screen quality counter を無効化するため、それらが必要な場合は `quality` または `combined` run を使ってください。

resource read 例:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "resources/read",
  "params": {
    "uri": "lookdevpt://actions/schema"
  }
}
```

## よく使う操作

camera の取得:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.get_state",
    "arguments": {}
  }
}
```

camera の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_camera",
    "arguments": {
      "position": [-14.7075, 7.99065, -11.7407],
      "yaw": 0.456,
      "pitch": -0.144733,
      "historyMode": "auto"
    }
  }
}
```

`historyMode` は、この camera mutation にだけ適用されます。

- `auto`（default）: 通常移動では history を再投影して維持し、大きな teleport / turn は camera cut と判定します。
- `preserve`: automatic cut threshold を超えても再投影を強制します。変更前後の view が意図的に連続している場合だけ使ってください。
- `reset`: 明示的な camera cut として扱い、新しい frame では temporal history を reject します。

Bistro の読み込み:

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_scene",
    "arguments": {
      "scenePath": "C:\\Projects\\D3D12LookDevPTWinUI\\Bistro_v5_2\\BistroExterior.fbx"
    }
  }
}
```

ReSTIR GI + DI に切り替え:

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_path_tracing",
    "arguments": {
      "mode": "restir_gi_di",
      "samplesPerFrame": 2,
      "maxBounces": 4,
      "radianceClamp": 8.0
    }
  }
}
```

interactive quality profile と automatic secondary shading budget の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_quality",
    "arguments": {
      "qualityProfile": "interactive_game",
      "restirBackend": "rtxdi",
      "secondaryShadingRate": "auto",
      "rayBudget": {
        "movingSpp": 1,
        "movingBounces": 2,
        "staticBaseSpp": 1,
        "staticMaxSpp": 2,
        "staticBounces": 4,
        "settleFrames": 8,
        "targetGpuMs": 14.5
      },
      "finalTaa": true,
      "sharpenStrength": 0.15,
      "referenceSpp": 4096
    }
  }
}
```

`secondaryShadingRate` は `auto`、`full`、`adaptive_half` を受理します。`auto` は追加 sample quota、bounce depth の順に削減し、それでも budget 超過が続いた場合に secondary shading を half-rate にします。回復は一定期間 budget を下回った場合だけ行います。`adaptive_half` は Interactive の secondary path を half-rate に固定し、`full` は無効化します。Sharp Preview / Reference Still では常に `full` に解決されます。partial `set_quality` は未指定値を維持しますが、profile 変更時はその profile の renderer / denoiser default を適用し、対象 history を reset します。

interactive denoise preset の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_denoise",
    "arguments": {
      "preset": "interactive_stable",
      "temporalStability": true,
      "jitterMode": "stable32",
      "movingJitterScale": 0.25,
      "resetHistory": true
    }
  }
}
```

NRD REBLUR を選択する例。NRD SDKとD3D12 evaluation resourceが利用可能ならactive backendになり、利用できない場合だけinternal denoiserへ安全にfallbackします。実際の経路は`lookdevpt.get_state`の`denoise.activeBackend`と`denoise.nrd.fallbackReason`を確認してください。setupは[Optional NVIDIA NRD Backend](nrd.ja.md)を参照してください。

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_denoise",
    "arguments": {
      "backend": "nrd_reblur",
      "resetNrd": true
    }
  }
}
```

DLSS Ray Reconstruction を選択する例。未対応環境では selected backend は保持しつつ internal denoiser に fallback します。詳細理由は `lookdevpt.get_state` の `denoise.dlss.fallbackReason` を確認してください。setup は [Optional DLSS Ray Reconstruction](dlss.ja.md) を参照してください。

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_denoise",
    "arguments": {
      "backend": "dlss_rr",
      "dlssMode": "quality",
      "resetDlss": true
    }
  }
}
```

material factor の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 17,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_material",
    "arguments": {
      "index": 0,
      "baseColor": [0.9, 0.76, 0.54, 1.0],
      "roughness": 0.42,
      "metallic": 0.0
    }
  }
}
```

material texture slot の override / clear:

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_material_texture",
    "arguments": {
      "index": 0,
      "slot": "baseColor",
      "path": "D:\\LookDevTextures\\paint_basecolor.png"
    }
  }
}
```

slot override を消す場合は `"clear": true`、import 元の texture に戻す場合は `"resetToSource": true` を使います。

material variant の保存と適用:

```json
{
  "jsonrpc": "2.0",
  "id": 17,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.save_material_variant",
    "arguments": {
      "index": 0,
      "variant": "warm rough"
    }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 18,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.apply_material_variant",
    "arguments": {
      "index": 0,
      "variant": "warm rough"
    }
  }
}
```

material focus と final view transform の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 19,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.run_actions",
    "arguments": {
      "actions": [
        {
          "method": "set_material_view",
          "params": { "selectedMaterial": 0, "focusMode": "dim" }
        },
        {
          "method": "set_color_management",
          "params": { "toneMapper": "aces", "exposure": 0.0, "gamma": 2.2 }
        }
      ],
      "validateOnly": false,
      "stopOnError": true
    }
  }
}
```

viewport capture:

```json
{
  "jsonrpc": "2.0",
  "id": 20,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.capture_viewport",
    "arguments": {}
  }
}
```

validation 付き batch 実行:

```json
{
  "jsonrpc": "2.0",
  "id": 21,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.run_actions",
    "arguments": {
      "actions": [
        {
          "method": "set_path_tracing",
          "params": { "mode": "restir_gi_di", "samplesPerFrame": 2 }
        },
        {
          "method": "set_denoise",
          "params": { "preset": "interactive_stable", "resetHistory": true }
        }
      ],
      "validateOnly": false,
      "stopOnError": true
    }
  }
}
```

debug review pack の capture:

```json
{
  "jsonrpc": "2.0",
  "id": 22,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.capture_debug_pack",
    "arguments": {
      "views": [
        "Final",
        "Base Color",
        "World Normal",
        "Roughness",
        "Metallic",
        "Direct Signal",
        "Indirect Signal",
        "History Confidence"
      ]
    }
  }
}
```

dialog なしで project 保存:

```json
{
  "jsonrpc": "2.0",
  "id": 23,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.save_project_as",
    "arguments": {
      "path": "C:\\Projects\\D3D12LookDevPTWinUI\\projects\\bistro.lookdevpt.json"
    }
  }
}
```

## Troubleshooting

- `401 Unauthorized`: `bearer_token` 認証が有効で、token がないか一致していません。WinUI の `MCP` panel から token を copy して client connection を再起動するか、server 起動前に明示的に `None` を選択してください。
- `403 Forbidden`: client が許可されていない `Origin` header を送っています。
- `400 Unsupported MCP-Protocol-Version`: `2025-11-25` または `2025-06-18` を使ってください。
- `400 MCP-Session-Id is required`: 最初に `initialize` を呼び、返ってきた `MCP-Session-Id` を送ってください。
- `404 Unknown MCP session`: session が削除されたか、app/server が再起動されています。再度 initialize してください。
- `GET` の `405 Method Not Allowed`: 想定通りです。この server は SSE を実装していません。
- `confirm_mutations` で mutation request が止まる: timeout 前に WinUI の `MCP` panel で Approve / Reject してください。
- `MCP mutation queue is full`: pending request が終わるのを待つか、pending mutation を approve/reject するか、server を再起動してください。
- mutation 成功直後の state / stats read が古く見える: state は 33 ms、stats / diagnostics は 100 ms 待ってから再度 read してください。
- `capture_debug_pack` が `read_only` で拒否される、または `confirm_mutations` で待つ: debug-view / history state を一時変更するため mutation access / approval が必要です。
- `lookdevpt://captures/latest.png` が読めない: 先に `lookdevpt.capture_viewport` を 1 回呼んでください。

## References

- [MCP lifecycle 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [VS Code MCP configuration reference](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)
