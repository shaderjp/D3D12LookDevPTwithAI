# Rendering load control during LLM inference

[日本語](ai-inference-render-coordination.ja.md)

## Overview

The integrated AI Assistant can run `llama-server.exe` with CUDA or Vulkan on
the same GPU as the DXR path tracer. The current implementation does not lower
the renderer quality profile during an Assistant turn. Instead, it temporarily
stops **recording, submitting, and presenting new DXR frames**.

The design has three goals:

- yield GPU compute and memory bandwidth to the local model;
- avoid advancing temporal parity, jitter, previous matrices, and sample counts
  when no corresponding frame is submitted; and
- keep MCP commands, reviews, cached snapshots, cancellation, and the Native UI
  responsive.

This is not a global GPU lock. Previously submitted work is not drained when
the gate changes, and operations such as a pending scene-resource refresh may
still issue GPU work. The precise guarantee is that steady-state viewport frame
submission is paused.

## Turn boundary

The pause begins after the Native UI successfully queues `sendTurn`, not after
the runtime reports its first generated token. It covers the complete turn:

1. runtime/model startup and status acquisition;
2. initial inference;
3. MCP tool execution;
4. one-time mutation approval waits;
5. continuation inference after tool results; and
6. final completion or error delivery.

The same conservative gate is used for CPU inference and short turns. The
current code does not select a policy from the inference backend, GPU adapter,
or live VRAM pressure.

## Signal path

```text
MainWindow::SendAssistantTurn
  -> successful PostAssistantRequest("sendTurn")
  -> RendererController::SetAssistantInferenceActive(true)
  -> atomic release store

RendererController::RenderMain  (renderer thread)
  -> atomic acquire load
  -> D3D12PathTracingBackend::SetAssistantInferenceActive(active)
  -> OnUpdate takes the paused frame-state path
  -> OnRender returns before command recording and submission

completed / error / host failure / host stop / window shutdown
  -> FinishAssistantTurnUi
  -> RendererController::SetAssistantInferenceActive(false)
```

[`RendererController`](../Source/WinUI/RendererController.cpp) uses
`std::atomic_bool m_assistantInferenceActive` across the UI/renderer thread
boundary. The UI performs a release store and the renderer loop performs an
acquire load. The backend's ordinary `bool` is then owned exclusively by the
renderer thread, so the UI thread never manipulates D3D12 objects directly.

## Renderer-thread behavior

### `RendererController::RenderMain`

The renderer thread itself remains alive. Each loop applies the gate and focus,
drains `RendererCommandQueue`, calls `OnUpdate()` and `OnRender()`, and publishes
an `EditorSnapshot` every 100 ms. While the gate is active it also sleeps for
16 ms to avoid busy-spinning. Keeping the loop alive gives MCP commands and
cached snapshots bounded processing latency.

### `D3D12PathTracingBackend::OnUpdate`

The pause check occurs after:

- pending resize handling;
- asynchronous scene-load polling;
- pending project, scene, and environment loads;
- pending GPU-resource refresh; and
- MCP command and benchmark-request processing.

The paused path updates `m_lastUpdate`, runs `ProcessMcpReview()` and
`UpdateMcpSnapshots()`, and returns before the normal per-frame state update.
This placement keeps scene audits and tools alive, but also means that pending
resource work can still use the GPU.

The skipped normal path includes camera delta application, frame-context and
constant-buffer updates, temporal jitter and previous/current matrix changes,
progressive sample advancement, and preparation for path tracing, denoising,
upscaling, and display passes.

### `D3D12PathTracingBackend::OnRender`

When the gate is active, `OnRender()` returns before:

- the frame-latency wait;
- `PopulateCommandList()`;
- `ID3D12CommandQueue::ExecuteCommandLists()`;
- swap-chain `Present()`;
- fence signaling and submitted-frame/index advancement; and
- benchmark capture and frame-completion accounting.

The viewport retains the last presented image. WinUI and the Assistant continue
on their own threads, so status, streamed text, tool cards, and cancellation
remain available.

## Temporal correctness

Running a normal `OnUpdate()` without a submitted GPU frame would advance CPU
history indices, jitter, previous matrices, and sample blocks without producing
the matching current histories. That can misalign TAA, denoiser, and ReSTIR
inputs when rendering resumes.

The paused path therefore:

- refreshes only `m_lastUpdate`, preventing the first resumed frame from seeing
  a delta equal to the full LLM turn;
- leaves temporal parity and jitter unchanged;
- does not advance submitted-frame or progressive-sample counters; and
- does not invalidate history merely because inference was active.

If an MCP or editor command changes the scene, camera, or quality settings, its
existing invalidation rules still apply. The first visual result is submitted
after the Assistant gate is released.

## Completion, cancellation, and failures

`FinishAssistantTurnUi()` centralizes release of the renderer gate. It is used
for successful/cancelled `completed` events, turn `error` events, bridge
`Failed`/`Stopped` states, Assistant shutdown, and window shutdown.

Pressing Stop does not release the gate immediately. The UI sends `cancelTurn`
and waits for a terminal event, or stops the owned host. This avoids claiming
that an already-started mutation did not run and keeps tool/result state
consistent through cancellation.

## Direct `lookdevpt.get_diagnostics` path

Separate from graphics control,
[`ChatCoordinator`](../Managed/D3D12LookDevPTwithAI.ChatHost/ChatCoordinator.cs)
has a fast path for an explicit diagnostics request. If the text starts with
`lookdevpt.get_diagnostics`, contains a run/call request, and contains no
recognized negation, ChatHost:

1. directly executes the read-only diagnostics tool without asking the model to
   select it;
2. appends the tool result to inference context; and
3. runs one tool-disabled inference round to explain the result.

This removes the usual initial-inference/tool/continuation round trip. It does
not remove the tool execution or final explanation time.

## Turn timing

Completion and error events report:

| Field | Meaning |
|---|---|
| `totalMilliseconds` | complete turn, including runtime, model, tools, and approval waits |
| `runtimeSetupMilliseconds` | runtime status and model/server startup |
| `initialInferenceMilliseconds` | model inference before tool results |
| `continuationInferenceMilliseconds` | model inference after tool results |
| `toolMilliseconds` | total MCP tool execution time |
| `inferenceRounds` / `toolCalls` | executed inference rounds and tool calls |

The Native UI renders these as `Last AI turn: ... total · runtime ... · model
... · tools ...`. They are wall-clock AI-turn measurements, not renderer GPU
timings. The pause duration is approximately the total turn time, with small UI
event-delivery and renderer-loop observation differences.

## Current trade-offs and limitations

- CPU inference and inference on another GPU still pause rendering.
- No `WaitForGpu()` is issued at gate activation, so the last submitted frame
  may briefly overlap model startup.
- The boolean assumes the current single-active-turn contract. Concurrent or
  background inference would require a generation ID or reference count.
- Resource refresh and tool handling can still issue GPU work while paused.
- A visual result from a tool mutation is not presented until the turn ends.
- There is currently no dedicated automated test that counts suppressed frame
  submissions; verification is source-contract plus Debug hardware testing.

Avoid starting an Assistant turn during a benchmark run: frame submission and
benchmark advancement pause together.

## Verification checklist

1. Load a GPU-heavy scene with CUDA or Vulkan inference configured.
2. Confirm progressive samples, camera animation, and GPU timing are advancing.
3. Send a long prompt and confirm the viewport stops advancing while Assistant
   status and MCP snapshots remain responsive.
4. Verify resume after normal completion, Stop/cancel, and a forced runtime
   error.
5. Check that the first resumed frame has no large camera delta or pause-induced
   TAA/denoiser parity mismatch.
6. Send `run lookdevpt.get_diagnostics and explain the result` and confirm tool
   and model time are reported separately.

## Implementation map

| File | Responsibility |
|---|---|
| [`Source/WinUI/MainWindow.Assistant.cpp`](../Source/WinUI/MainWindow.Assistant.cpp) | activate on turn start, release on every terminal path, display timings |
| [`Source/WinUI/RendererController.h`](../Source/WinUI/RendererController.h) | atomic UI/renderer thread flag |
| [`Source/WinUI/RendererController.cpp`](../Source/WinUI/RendererController.cpp) | forward the flag, publish snapshots, 16 ms paused sleep |
| [`Source/D3D12PathTracingBackend.h`](../Source/D3D12PathTracingBackend.h) | renderer-thread-local active flag |
| [`Source/D3D12PathTracingBackend.cpp`](../Source/D3D12PathTracingBackend.cpp) | paused temporal-update path and frame-submission gate |
| [`Managed/D3D12LookDevPTwithAI.ChatHost/ChatCoordinator.cs`](../Managed/D3D12LookDevPTwithAI.ChatHost/ChatCoordinator.cs) | direct diagnostics path and turn timing aggregation |
| [`Managed/D3D12LookDevPTwithAI.Chat.Core/PipeProtocol.cs`](../Managed/D3D12LookDevPTwithAI.Chat.Core/PipeProtocol.cs) | completion/error timing IPC contract |

## Possible follow-ups

- Pause only when inference and rendering use the same adapter.
- Offer policies such as full pause, low render scale/ray budget, or an FPS cap.
- Expose gate transitions, submitted serials, fence state, suppressed frames,
  pause duration, and resume latency in local diagnostics.
- Add automated success, cancel, error, and host-failure submission-count tests.
