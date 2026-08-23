# 統合公開ベータ受け入れチェックリスト

対象は `BuildIntegratedPortable.ps1` で生成した現在の one-app Release x64 payload です。
旧 `0.2.0-beta.1` の `LocalMCPChatClient`、pairing code、projector / vision model を使う
2アプリ構成のチェックリストではありません。自動 test に合格しても、この実機確認が完了する
までは公開用 tag を push しません。

`BuildNvidiaRelease.ps1`で生成するNVIDIA renderer backend同梱payloadは別の配布境界で、
このチェックリストの対象外です。license、NGX Application ID、対象GPU、file台帳の確認は
[NVIDIA開発・Release setup](nvidia-setup.ja.md)のRelease checklistを使用してください。

## 必須環境

- Visual Studio、.NET Runtime、Windows App Runtime を追加導入していない Windows 11 x64
- NVIDIA DXR GPU 搭載機 1 台
- AMD または Intel DXR GPU 搭載機 1 台
- 標準 user 権限
- payload、選択 model / runtime、download 用 `.partial` を含められる十分な空き容量

## One-app 導入シナリオ

1. ZIP と配布された SHA-256 を照合し、user folder へ展開する。
2. `D3D12LookDevPTwithAI.exe` を直接起動する。別 launcher、PowerShell、Visual Studio、
   LocalMCPChatClient、system-wide .NET Runtime を要求しないことを確認する。
3. AI Assistant に `Local assistant is ready` が表示されることを確認する。同梱 model がない
   payload では `Loaded model: none` は正常な遅延 load 状態であり、推論失敗とは扱わない。
4. `Install or change local model` を開き、Gemma 4、CPU / CUDA / Vulkan backend、両 license
   link、明示同意 checkbox を確認する。download 中は current item、受信量 / 総量、全体
   percentage、Cancel が更新されることを確認する。cancel 後に同じ setup を再開し、
   `.partial` から再開できることを確認する。model download 時間は導入所要時間から除外する。
5. Bistro または PBRT scene を開き、`Ctrl+Enter` で prompt を送る。入力欄が空になり、
   loaded model 名 / backend と青い思考中表示が更新され、Stop が有効になることを確認する。
6. `Describe scene` または同等の read-only request で MCP Tool が自動実行され、最終回答が
   同じ dock に表示されることを確認する。
7. 露出などの変更を依頼し、canonical JSON 引数を含む一回承認 card が表示されることを
   確認する。Allow once / Deny の両経路を試し、許可時だけ renderer state が変わることを
   `get_state` または Inspector で確認する。

![露出変更を一回承認して実行した統合AI Assistant](images/screenshot009.png)

## 機能・安全性

- AI Assistant は text chat と MCP state / diagnostics による Tool 操作であり、viewport
  pixel を model へ渡していないことを UI / documentation が明示する。
- 起動直後、model 起動中、生成中、Tool 実行中、承認待ち、cancel、失敗、完了の状態を
  見分けられる。失敗時は API key、token、local artifact path、server body を表示しない。
- model / runtime artifact の size と SHA-256、runtime tree の完全一致を検証し、不一致時は
  llama-server を起動しない。
- app 終了後に `D3D12LookDevPTwithAI.ChatHost.exe` と `llama-server.exe` が残留しない。
- MCP の `read_only`、`confirm_mutations`、`allow_mutations` を確認し、公開ベータ既定は
  `confirm_mutations` とする。外部 MCP client 接続は追加機能であり、統合 Assistant の
  必須導入手順には含めない。
- benchmark の完了 / cancel 後に camera、quality、VSync が元へ戻る。
- 2時間反復後に orphan process、秘密情報を含む設定 / log / diagnostics、再現可能な crash がない。
- payload manifest、file 単位 license map、SPDX SBOM が配布 file を網羅し、Debug layer、
  symbol、credential、承認状態、会話履歴、user settings を含めない。

## 記録

各 GPU について Windows build、GPU、driver、application commit、payload / archive hash、
AI artifact ID / revision、backend、開始・終了時刻、合否、発見した P0 / P1 issue を記録します。
公開条件は両 GPU で合格し、P0 / P1 が 0 件であることです。
