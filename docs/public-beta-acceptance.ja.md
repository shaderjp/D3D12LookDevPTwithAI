# 公開ベータ受け入れチェックリスト

対象は`D3D12LookDevSuite-0.2.0-beta.1-win-x64.zip`です。自動テストに合格しても、
この実機確認が完了するまではGitHub Pre-release用tagをpushしません。

## 必須環境

- Visual Studio、.NET、Windows App Runtimeを追加導入していないWindows 11 x64
- NVIDIA DXR GPU搭載機1台
- AMDまたはIntel DXR GPU搭載機1台
- 標準ユーザー権限と5 GiB以上の空き容量

## 15分導入シナリオ

1. ZIPと`.sha256`を照合してユーザーフォルダーへ展開する。
2. `Launch-LookDevSuite.ps1`を実行し、診断JSONが作成されることを確認する。
3. D3D12側に表示された8桁コードをLocalMCPChatClient初回画面へ入力する。
4. 推奨runtime、Gemma 4、mmprojを取得し、hash検証が完了することを確認する。
5. モデルdownload時間を除き、展開開始から15分以内にquick reviewが完了することを確認する。

## 機能・安全性

- quick / material / lighting / temporalの各reviewで画像、metric、進捗を確認する。
- 画像を拡大・保存でき、text-only時には「モデルは画像を見ていない」と表示される。
- pairing token失効後は接続が拒否され、新しい8桁コードで再pairingできる。
- 安全な変更を採用・復元でき、停止時にもcheckpointが残らない。
- benchmarkの完了と中止後にcamera、quality、vsyncが元へ戻る。
- 2時間反復後に孤立したllama/D3D12 process、秘密情報を含む設定・log・診断、再現可能なcrashがない。
- uninstall既定動作で設定、会話、artifact、Credential Manager項目が保持され、
  `-RemoveLocalApplicationData`指定時だけlocal dataが削除される。

## 記録

各GPUについてWindows build、GPU、driver、Suite hash、開始・終了時刻、合否、発見した
P0/P1 issueを記録します。公開条件は両GPUで合格し、P0/P1が0件であることです。
