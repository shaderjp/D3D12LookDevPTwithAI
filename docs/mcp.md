# MCP Server

D3D12LookDevPTwithAI includes a local MCP server for inspecting and controlling the running renderer from tools such as VS Code, Codex, or custom JSON-RPC clients. The server and WinUI editor share the same validation-oriented renderer-command layer.

Japanese documentation: [MCP サーバー](mcp.ja.md)

## MCP-Driven Workflow Example

An MCP-capable client can issue camera, quality, material, or denoise requests.
The WinUI MCP panel shows sessions, pending approvals, and recent local
JSON-RPC requests. Do not commit real MCP tokens in screenshots or project
files.

The client should validate settings first, apply mutation tools, then read state back to confirm that the renderer accepted the same values. Treat the live schema returned by `tools/list` or `lookdevpt://actions/schema`—and the examples below—as authoritative for the current build.

## Availability And Security

- Endpoint: `http://127.0.0.1:<port>/mcp`
- Default port: `8777`
- Bind address: `127.0.0.1` only
- Transport: Streamable HTTP-style JSON-RPC over `POST /mcp`
- Protocol versions accepted: `2026-07-28`, `2025-11-25`, `2025-06-18`
- LookDev contract: `initialize.experimental.lookdevpt.contractVersion = "1.0"`
- Authentication: `bearer_token` (default) or `none`; `bearer_token` requires `Authorization: Bearer <token>`
- Session: legacy initialize returns `MCP-Session-Id`; `2026-07-28` is stateless
- Server-Sent Events: `POST subscriptions/listen` streams subscribed Resource
  updates; standalone `GET /mcp` returns `405 Method Not Allowed`
- Maximum HTTP request body: 16 MiB
- HTTP/1.1 request bodies accept either `Content-Length` or `Transfer-Encoding: chunked`. Chunk extensions and trailers are safely consumed; ambiguous requests containing both framing headers are rejected.

The authentication mode, credential reference, and other MCP settings are
stored in:

```text
%APPDATA%\D3D12LookDevPTwithAI\settings.json
```

The primary bearer token itself is stored in Windows Credential Manager;
paired-client records contain only SHA-256 token hashes. Legacy settings that
contain a plain-text primary token are migrated on first start. Do not copy a
token into `.lookdevpt.json`, README files, screenshots, issue comments, or
committed VS Code settings.

The server accepts browser/client `Origin` values only when absent, `null`, `http://127.0.0.1:*`, or `http://localhost:*`. Other origins are rejected with `403`.

## Starting The Server

Use the dockable `MCP Server` panel:

- `Start Server` / `Stop Server`
- `Port`
- `Request Timeout`
- `Access Mode`
- `Authentication`
- `Copy Token`
- `Regenerate Token`
- `Pair LocalMCPChatClient` and paired-client revocation
- `Export mcp.json...`
- pending approvals and recent request log

`Export mcp.json...` writes a VS Code-compatible MCP configuration containing
the current endpoint and protocol version. By default it uses a password-style
`inputs` prompt and does not write the bearer token. Enable `Embed bearer token
in exported mcp.json` only when a self-contained local configuration is
required; the token is then stored as plain text, so keep that file outside
source control and do not share it. Request timeout and access mode remain
application-local server settings and are not part of VS Code's `mcp.json`
schema. When `Authentication` is `None`, the exported file omits both `inputs`
and the `Authorization` header.

The server is disabled by default. It can also be started from the command line:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe --mcp-server --mcp-port 8777 --mcp-auth bearer_token --mcp-token <token> --mcp-access confirm_mutations
```

Access modes:

- `read_only`: read tools work; mutation tools are rejected.
- `confirm_mutations`: mutation tools wait for approval in the WinUI `MCP` panel.
- `allow_mutations`: mutation tools execute without UI approval.

Authentication modes:

- `bearer_token`: the default. Every request must carry the configured bearer token.
- `none`: requests do not require an `Authorization` header. The server remains fixed to `127.0.0.1`; this mode is not exposed on external interfaces. Prefer `read_only` or `confirm_mutations` when authentication is disabled.

To start without authentication from the command line:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe --mcp-server --mcp-port 8777 --mcp-auth none --mcp-access confirm_mutations
```

The mutation queue is processed at a renderer-thread safe point and has a limit of 16 queued requests. Mutations never touch D3D12 or WinUI state directly from the HTTP server thread.

### LocalMCPChatClient pairing

The panel generates an 8-digit, one-time code that expires after 90 seconds
and is invalidated after five failed attempts. A loopback client discovers the
contract with `GET /.well-known/lookdevpt/v1` and exchanges the code with
`POST /pair`. The server returns a client-specific 256-bit bearer token and
stores only its hash; LocalMCPChatClient stores the token in Windows Credential
Manager. Discovery, pairing, and MCP all remain bound to `127.0.0.1` and share
the same Origin validation. Revoke a paired client from the MCP panel to make
its token fail immediately.

`capture_viewport` is a read operation, but it is still queued on the renderer thread because it performs a GPU readback. `capture_debug_pack` temporarily changes the debug view and invalidates its related temporal history, so it requires mutation access (and approval in `confirm_mutations` mode). Its `restoreView` option defaults to `true`.

## Snapshot Cadence And Freshness

MCP reads use mutex-protected snapshots rather than walking renderer or scene data on the HTTP thread. Snapshot work is disabled while the server is stopped. Starting the server forces an initial snapshot; while it is running:

- `state` is refreshed about every 33 ms (30 Hz).
- `stats` and `diagnostics` are refreshed about every 100 ms (10 Hz).
- `materials`, `project`, `scene/summary`, material variants, and presets are rebuilt only when their scene/project/catalog revisions change.
- Debug-view, render-mode, and action-schema resources are generated from fixed metadata on demand.

A read immediately following a mutation can therefore normally trail the renderer by one refresh interval. If a client must verify a value, read `get_state` again after that interval while the renderer is advancing frames. Mutation, validation, and capture operations are serialized through the renderer command queue; ordinary immutable snapshot reads do not block renderer state mutation.

## VS Code Configuration

VS Code stores MCP server configuration in `mcp.json`, either in `.vscode/mcp.json` or in the user profile. VS Code's current MCP configuration reference uses `type`, `url`, and `headers` for HTTP servers, with optional `inputs` for secrets.

Example `.vscode/mcp.json`:

```json
{
  "inputs": [
    {
      "type": "promptString",
      "id": "lookdevpt-token",
      "description": "D3D12LookDevPTwithAI MCP bearer token",
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

With `Authentication` set to `None`, omit the token input and authorization
header:

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

Use `MCP: List Servers` to start or restart the server entry after editing the file. Use `MCP: Reset Cached Tools` if the tool list changes after rebuilding D3D12LookDevPTwithAI.

Notes:

- Start D3D12LookDevPTwithAI and its MCP server before starting the VS Code MCP entry.
- With `bearer_token`, if the token is regenerated in the WinUI MCP panel, restart the VS Code MCP server entry and enter the new token.
- This server supports HTTP POST JSON-RPC and POST-based subscription SSE.
  Clients that require a standalone GET SSE transport will not work.

## JSON-RPC Flow

Every client should initialize first, keep the returned session id, then send `notifications/initialized`.

PowerShell example:

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

When the server uses `Authentication: None`, omit `Authorization` from both
`$headers` and `$sessionHeaders`; the session and protocol headers are still
required as shown.

Call a read tool:

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

End a session:

```powershell
Invoke-WebRequest -Uri $endpoint -Method Delete -Headers $sessionHeaders
```

## Tools

Read tools:

- `lookdevpt.get_stats`: returns adapter, DXR tier, resolution, aggregate GPU timing, scene counts, history/resource-memory status, active secondary shading rate, denoiser status, and MCP queue state.
- `lookdevpt.get_state`: returns scene/project paths, quality and ray-budget settings, camera, lighting, path tracing, ReSTIR/RTXDI status, denoise, frame-history revisions, and view state.
- `lookdevpt.list_materials`: returns stable source material ids, editable core/glTF extension factors, all 14 texture bindings, source/resident dimensions, transcode format, resident bytes, and fallback state.
- `lookdevpt.list_debug_views`: returns debug view ids, labels, and keys.
- `lookdevpt.list_render_modes`: returns render mode labels and action values.
- `lookdevpt.get_diagnostics`: returns scene/project/capture/MCP diagnostics.
- `lookdevpt.capture_viewport`: captures the current final/debug viewport as PNG and returns an inline `image/png` plus `lookdevpt://captures/latest.png`.
- `lookdevpt.audit_scene`: returns cached, stable-code diagnostics for scene structure, geometry, glTF extensions used/required/unsupported, materials, texture transcodes/residency/VRAM, lighting, and RTXDI/NRD/DLSS fallback state.
- `lookdevpt.probe_surfaces`: traces up to 16 exact surface probes in normalized or output-pixel coordinates without writing the normal render targets or temporal history.
- `lookdevpt.compare_captures`: compares two same-resolution captures using linear-sRGB RMSE/PSNR, luminance SSIM, maximum difference, changed-pixel ratio, fingerprints, and a heatmap.
- `lookdevpt.start_review`, `lookdevpt.get_review`, and `lookdevpt.cancel_review`: run and control one asynchronous `quick`, `material`, `lighting`, or `temporal` review. Reviews are available in `read_only` mode and do not change camera, debug view, accumulation, temporal history, or project dirty state.

Validation and capture workflow tools:

- `lookdevpt.validate_action`: accepts `{ "method": "...", "params": { ... } }` and runs the same action path with `validateOnly=true`.
- `lookdevpt.run_actions`: validates and applies up to 16 action-layer calls as one MCP request. Validation failure prevents all mutation. Application is ordered but not rollback-transactional if a later runtime operation fails.
- `lookdevpt.capture_debug_pack`: captures up to eight debug views and returns resource links for each PNG. It is treated as a mutation because it changes debug-view/history state while capturing.

Safe-change and benchmark tools:

- `lookdevpt.create_checkpoint`: stores camera, lighting, materials, quality,
  denoise, and view state for the current scene fingerprint.
- `lookdevpt.restore_checkpoint`: restores the state only if the current scene
  fingerprint still matches.
- `lookdevpt.delete_checkpoint`: deletes a stored checkpoint.
- `lookdevpt.start_benchmark`: starts one asynchronous BenchmarkHarness run
  after checkpointing the interactive renderer state.
- `lookdevpt.get_benchmark`: reports progress, GPU/quality metrics, and result
  artifact links.
- `lookdevpt.cancel_benchmark`: cancels the run and restores the checkpointed
  interactive state.

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

Tool results primarily use `structuredContent`. A text content summary is also included for compatibility.

## Resources

- `lookdevpt://integration`: application/contract versions, supported review,
  safe-change and benchmark capabilities, `gltfMaterialExtensionsV1`,
  `textureResidencyV1`, and artifact limits. The LookDev contract remains 1.0.
- `lookdevpt://state`: current state JSON.
- `lookdevpt://stats`: current stats JSON.
- `lookdevpt://diagnostics`: scene, project, capture, and MCP diagnostics.
- `lookdevpt://materials`: material list JSON.
- `lookdevpt://materials/{index}`: one material object.
- `lookdevpt://materials/{index}/textures`: source/current/override texture slots for one material.
- `lookdevpt://material-variants`: saved per-material variant snapshots.
- `lookdevpt://material-presets`: built-in and user material presets.
- `lookdevpt://debug-views`: debug view ids, labels, and keys.
- `lookdevpt://render-modes`: render modes and `set_path_tracing.mode` values.
- `lookdevpt://project`: current project path and dirty flag.
- `lookdevpt://scene/summary`: scene counts, bounds, lights, and asset paths.
- `lookdevpt://scene/audit`: cached scene and renderer-fallback audit.
- `lookdevpt://actions/schema`: action names and JSON input schemas.
- `lookdevpt://captures/index`: in-memory capture history.
- `lookdevpt://captures/latest.png`: most recent PNG capture.
- `lookdevpt://captures/{id}.png`: PNG from `capture_viewport` or `capture_debug_pack`.
- `lookdevpt://reviews/index`: review queue/history and active review id.
- `lookdevpt://reviews/{id}`: review state, progress, audit, captures, and optional comparison.
- `lookdevpt://comparisons/{id}`: comparison metrics and scene/camera fingerprint matches.
- `lookdevpt://comparisons/{id}/heatmap.png`: PNG difference heatmap.
- `lookdevpt://probes/latest`: most recent surface-probe result.
- `lookdevpt://checkpoints/index`: checkpoint ids, labels, and fingerprints.
- `lookdevpt://checkpoints/{id}`: one checkpoint's metadata.
- `lookdevpt://benchmarks/index`: asynchronous benchmark history and progress.
- `lookdevpt://benchmarks/{id}`: one benchmark's progress, metrics, and links.
- `lookdevpt://benchmarks/{id}/{artifact}`: `summary.json`, `artifacts.json`,
  `quality_analysis.json`, or `frames.csv`.

Resource templates:

- `lookdevpt://captures/{id}.png`
- `lookdevpt://materials/{index}`
- `lookdevpt://materials/{index}/textures`
- `lookdevpt://reviews/{id}`
- `lookdevpt://comparisons/{id}`
- `lookdevpt://comparisons/{id}/heatmap.png`
- `lookdevpt://checkpoints/{id}`
- `lookdevpt://benchmarks/{id}`
- `lookdevpt://benchmarks/{id}/{artifact}`

Prompts:

- `lookdevpt.inspect_scene`: read state/stats/materials/diagnostics and summarize the scene.
- `lookdevpt.tune_denoise`: propose and apply stable denoise settings through validation.
- `lookdevpt.setup_camera_shot`: fit/refine a camera shot using scene bounds and state.
- `lookdevpt.capture_debug_review`: capture a debug pack and summarize visible issues.
- `lookdevpt.review_scene`: perform a non-destructive audit/review/probe workflow.
- `lookdevpt.review_change`: review current output against a baseline capture and explain fingerprint mismatches separately.

Review captures use an asynchronous GPU fence and one submitted capture per frame. PNG conversion runs on a worker. Capture and heatmap artifacts stay in memory and are limited to 24 images or 128 MiB with LRU eviction; active-review artifacts are pinned.

## State, Stats, And Benchmark Metrics

Important `get_state` fields added for the stability/performance pipeline are:

- `quality`: the stored/requested `qualityProfile`, `restirBackend`, `secondaryShadingRate`, complete `rayBudget`, `finalTaa`, `sharpenStrength`, and `referenceSpp` policy object. Profile- and availability-dependent results are reported separately by `finalTaaActive`, `restir.effective`, and `denoise.activeBackend`.
- `finalTaaActive`: whether the selected profile and available pipeline are actually running Final TAA.
- `pathTracing.requestedSecondaryShadingRate`, `activeSecondaryRate`, and `autoSecondaryHalfActive`: requested policy versus the currently active secondary rate.
- `restir.requestedBackend`, `effective`, and `rtxdiStatus`: requested RTXDI path, compiled/runtime availability, active DI/GI state, and fallback reason.
- `frameState`: frame/sample counters, change mask, valid history-domain mask, camera-cut flag, and independent scene/geometry/material/light/HDRI/backend/profile revisions.

Important `get_stats` groups are:

- `gpuTiming`: last completed aggregate pipeline, Path Trace, ReSTIR reuse, denoise, copy, and UI timings, together with validity and completion serial.
- `historyDomains`: valid history mask and the last change mask.
- `resourceMemory`: frame/history bytes and MiB, the 512 MiB budget, budget status, and active allocation profile.
- `secondaryShading`: requested/effective rate, active ratio, automatic half-rate state, additional-sample quota, bounce penalty, and over/under-budget counters.
- `denoiser` and `mcp`: effective backend/history status and server queue state.

The benchmark CSV/JSON artifacts contain the heavier per-phase metrics—ReSTIR candidate/temporal/spatial/shade/publish, denoise prepare/core/composite, Final TAA, quality counters, history publish, detailed CPU stages, estimated ray budgets, and history/contribution diagnostics. `get_stats` intentionally remains a lower-cost aggregate snapshot. In a `performance` benchmark, full-screen quality counters are disabled; use a `quality` or `combined` run when those counters are required.

Example resource read:

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

## Common Operations

Get camera:

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

Set camera:

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

`historyMode` controls only this camera mutation:

- `auto` (default): preserve and reproject history for ordinary motion, but classify a large teleport/turn as a camera cut.
- `preserve`: force reprojection even when the automatic cut threshold would be exceeded. Use only when the previous and new views are intentionally continuous.
- `reset`: mark an explicit camera cut and reject temporal history for the new frame.

Load Bistro:

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_scene",
    "arguments": {
      "scenePath": "C:\\Projects\\D3D12LookDevPTwithAI\\Bistro_v5_2\\BistroExterior.fbx"
    }
  }
}
```

Set ReSTIR GI + DI:

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

Set the interactive quality profile and automatic secondary shading budget:

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

`secondaryShadingRate` accepts `auto`, `full`, or `adaptive_half`. `auto` spends the additional-sample quota and then reduces bounce depth before enabling half-rate secondary shading after sustained budget overruns; recovery occurs only after a sustained under-budget period. `adaptive_half` forces the interactive secondary path to half rate, while `full` disables it. Sharp Preview and Reference Still always resolve this field to `full`. Partial `set_quality` calls preserve unspecified values, but changing profiles applies that profile's renderer/denoiser defaults and resets the affected histories.

Set the interactive denoise preset:

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

Select NRD REBLUR. When the NRD SDK and D3D12 evaluation resources are available, this becomes the active backend; otherwise the renderer safely falls back to the internal denoiser. Inspect `denoise.activeBackend` and `denoise.nrd.fallbackReason` from `lookdevpt.get_state` for the effective path. More setup notes are in [Optional NVIDIA NRD Backend](nrd.md).

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

Select DLSS Ray Reconstruction when available. Unsupported machines keep the selected backend but fall back to the internal denoiser; read `denoise.dlss.fallbackReason` from `lookdevpt.get_state` for details. More setup notes are in [Optional DLSS Ray Reconstruction](dlss.md).

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

Set material and glTF extension factors:

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
      "metallic": 0.0,
      "gltfExtensions": {
        "specularFactor": 0.8,
        "ior": 1.52,
        "transmissionFactor": 0.65,
        "thicknessFactor": 0.012,
        "attenuationColor": [0.82, 0.95, 1.0],
        "attenuationDistance": 0.4,
        "clearcoatFactor": 0.2,
        "clearcoatRoughnessFactor": 0.08
      }
    }
  }
}
```

Override a material texture slot and its non-destructive glTF binding:

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_material_texture",
    "arguments": {
      "index": 0,
      "slot": "clearcoatNormal",
      "path": "D:\\LookDevTextures\\coat_normal.ktx2",
      "uvSet": 1,
      "offset": [0.0, 0.0],
      "scale": [2.0, 2.0],
      "rotation": 0.25,
      "sampler": "linearMirror",
      "resolutionPolicy": "auto"
    }
  }
}
```

Use `"clear": true` to remove the slot override, or `"resetToSource": true` to restore the imported source texture for that slot.

Save and apply a material variant:

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

Focus one material and adjust the final view transform:

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

Capture the viewport:

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

Run a validated batch:

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

Capture a debug review pack:

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

Save a project without a dialog:

```json
{
  "jsonrpc": "2.0",
  "id": 23,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.save_project_as",
    "arguments": {
      "path": "C:\\Projects\\D3D12LookDevPTwithAI\\projects\\bistro.lookdevpt.json"
    }
  }
}
```

## Troubleshooting

- `401 Unauthorized`: `bearer_token` authentication is enabled and the token is missing or does not match. Copy the token from the WinUI `MCP` panel and restart the client connection, or explicitly select `None` before starting the server.
- `403 Forbidden`: client sent a disallowed `Origin` header.
- `400 Unsupported MCP-Protocol-Version`: use `2025-11-25` or `2025-06-18`.
- `400 MCP-Session-Id is required`: call `initialize` first, then send the returned `MCP-Session-Id`.
- `404 Unknown MCP session`: the session was deleted or the app/server restarted. Initialize again.
- `405 Method Not Allowed` on `GET`: expected; subscriptions use
  `POST subscriptions/listen`, not a standalone GET transport.
- Mutation request hangs in `confirm_mutations`: approve or reject it in the WinUI `MCP` panel before the request timeout.
- `MCP mutation queue is full`: wait for pending requests to finish, approve/reject pending mutations, or restart the server.
- A state/stat read appears stale after a successful mutation: wait 33 ms for state or 100 ms for stats/diagnostics, then read again.
- `capture_debug_pack` is rejected in `read_only` or waits in `confirm_mutations`: the tool temporarily changes debug-view/history state and therefore requires mutation access/approval.
- `lookdevpt://captures/latest.png` fails: call `lookdevpt.capture_viewport` once before reading the resource.

## References

- [MCP lifecycle 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [VS Code MCP configuration reference](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)
