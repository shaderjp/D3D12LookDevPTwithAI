# D3D12LookDevPTwithAI 統合アーキテクチャ

## 目的

`D3D12LookDevPTwithAI` は、LookDev エディターとローカル AI アシスタントを単一の WinUI 3 アプリとして提供する。展示時の操作対象はひとつのウィンドウだけとし、利用者は右ドックの AI Assistant からシーン解析、カメラ調整、レンダリング設定、比較、ベンチマークを自然言語で実行できる。

AI 推論、会話履歴、MCP 通信はすべてローカルで完結する。クラウド API は必須にしない。

## 製品境界

```text
D3D12LookDevPTwithAI.exe  (唯一の表示アプリ)
  ├─ D3D12 renderer / editor
  ├─ embedded LookDev MCP server
  ├─ AI Assistant right dock
  └─ current-user named pipe server
       └─ D3D12LookDevPTwithAI.ChatHost.exe  (非表示の子プロセス)
            ├─ local conversation store
            ├─ tool planner / approval broker
            ├─ LookDev MCP client (このアプリの endpoint だけ)
            └─ llama.cpp server/process  (必要時のみ、非表示)
```

`ChatHost` と `llama.cpp` は障害分離とランタイム分離のための内部プロセスであり、別アプリとして利用者へ露出しない。親プロセスが所有する Job Object に入れ、アプリ終了時に残留させない。

## UI

- 既存のエディターを主役のまま維持する。
- 右ペインに `Inspector` / `AI Assistant` のモード切替を置く。
- 新製品の初期モードは `AI Assistant`、推奨幅は 420 px とする。
- `F9` で Assistant を開閉し、`F10` の render-only 動作は維持する。
- Assistant は会話、モデル状態、固定クイック操作、ストリーミング応答、Tool 実行状態、一回承認カードを表示する。
- 変更操作の承認は `今回のみ承認` と `拒否` の2択とし、永続許可は設けない。

## IPC

C++ 側を Named Pipe server、Managed ChatHost を client とする。

- current-user ACL と `PIPE_REJECT_REMOTE_CLIENTS` を使用する。
- 双方で相手 PID を検証する。
- frame は `uint32 little-endian length + UTF-8 JSON` とし、最大 4 MiB に制限する。
- request は即時応答し、長時間の生成や Tool 実行は event として非同期配信する。
- pipe reader は生成完了を待たず、取消と承認応答を常に処理できるようにする。
- MCP bearer token、会話本文、Tool 引数を通常ログへ出さない。

第一版の request:

- `initialize`
- `conversation.list`
- `conversation.create`
- `conversation.select`
- `sendTurn`
- `cancelTurn`
- `approval.respond`
- `shutdown`

第一版の event:

- `runtimeState`
- `messageAdded`
- `textDelta`
- `toolApprovalRequired`
- `toolStarted`
- `toolCompleted`
- `completed`
- `error`

## MCP と承認

既存の外部 MCP 契約は互換のまま維持する。

- Tool / Prompt: `lookdevpt.*`
- Resource: `lookdevpt://...`
- HTTP: `/mcp`, `/pair`, `/.well-known/lookdevpt/v1`
- server info と protocol / contract version

統合 Assistant は、そのアプリ自身が起動した loopback MCP endpoint 以外へ接続しない。MCP が返す `readOnlyHint=true` の Tool だけを自動実行し、それ以外は必ず UI の一回承認を通す。

二重承認を避けるため、許可時に Native 側が次の情報へ結び付いた短寿命・一回限りの grant を発行する。

- 専用 MCP session
- Tool 名
- canonical JSON arguments hash
- 30 秒以内の有効期限

ChatHost は grant を MCP request の `_meta.shaderjp.lookdevpt/approvalToken` へ付与する。LookDev MCP は一致した grant を一度だけ消費し、既存 `McpDispatcher` の追加承認を省略する。不一致、期限切れ、再利用は即座に拒否する。外部クライアントの従来承認フローは変更しない。

## ローカル AI

- 標準モデルは Gemma 4 E2B IT の Q4 GGUF とする。
- 初回起動時にアプリからダウンロードでき、展示向けには同じ manifest と hash を使う offline pack を用意する。
- GPU memory budget を renderer と共有し、余裕がない場合は context、batch、GPU offload を自動調整する。
- モデルと llama.cpp artifact は署名済み manifest / SHA-256 で検証してから有効化する。
- ダウンロード失敗や推論プロセス異常は renderer を落とさず、Assistant ペインだけを再試行可能な状態へ戻す。

## 保存

- 製品設定: `%APPDATA%\D3D12LookDevPTwithAI`
- AI artifact: `%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI`
- 会話履歴: project context key で分離した SQLite
- MCP token: ChatHost へ初期化時だけ渡すメモリ内 secret。ChatHost は永続化しない。

project context key は正規化した project path / scene path から生成する。同名ファイルを別ディレクトリで開いた場合、Save As、未保存 preview を区別する。

## スレッド境界

- render thread は immutable な editor snapshot を公開する。
- UI thread は snapshot から最小の renderer context を作り、bridge queue へ送る。
- pipe reader thread は immutable event を生成し、`DispatcherQueue` で UI thread へ渡す。
- pipe thread と ChatHost は XAML や renderer object を直接操作しない。
- Tool による変更はすべて既存 MCP command queue を経由する。

## 配布

利用者が起動する executable と shortcut は `D3D12LookDevPTwithAI` のみとする。portable / installer には内部 ChatHost、.NET runtime、llama.cpp、ライセンス、モデル manifest を同梱するが、旧 `LocalMCPChatClient` launcher は含めない。

展示用 offline pack は、ネットワークなしで次を検証・展開できるようにする。

- renderer と ChatHost
- Windows App Runtime / .NET runtime
- llama.cpp backend
- 標準 GGUF / mmproj
- third-party notices と source offer

## 実装マイルストーン

1. 製品改名と基盤移植
2. Native / Managed IPC と右ドックの縦断
3. 単一 loopback MCP 接続、readonly 自動実行、一回承認 grant
4. llama.cpp 推論、GPU budget、モデル setup
5. project 別履歴、クイック操作、demo reset
6. one-app portable / offline pack、障害試験、展示 acceptance test

各マイルストーンで `Debug|x64`、IPC protocol tests、MCP tests、Visual Studio filter の一対一整合性を検証する。
