# MCP HTTP相互運用性 修正指示書

## 目的

`D3D12LookDevPTWinUI` のMCPサーバーを、`Content-Length` 固定のクライアントだけでなく、標準的なHTTP/1.1クライアントおよび `ModelContextProtocol` C# SDK 2.0.0から直接接続できる実装へ修正する。

修正後は、クライアント側でHTTP本文を事前バッファする回避策を有効にしなくても、次の処理が成功すること。

1. `initialize`
2. `notifications/initialized`
3. `tools/list`
4. 読み取り専用ツール `lookdevpt.get_stats`
5. セッション終了の `DELETE /mcp`

## 確認済みの障害原因

対象は `Source/McpServer.cpp` の `Server::ReadHttpRequest`。

現在の実装は `Content-Length` だけを本文境界として扱っている。`Transfer-Encoding: chunked` のリクエストでは `Content-Length` がないため本文長を0と判断し、chunked framingを含む受信済みデータも0バイトへ切り詰める。その結果、JSONパーサーへ空文字列が渡り、次の応答になる。

```text
HTTP 400
JSON-RPC -32700: Unexpected end of JSON.
```

同じ認証情報とJSON-RPC本文を `Content-Length` 付きで送ると、初期化、ツール一覧取得、36ツールの登録、`lookdevpt.get_stats` は成功する。認証、MCPセッション、ツール実装ではなく、HTTP/1.1のリクエスト本文framingが原因である。

HTTP/1.1受信側はchunked transfer codingを解析できる必要がある。詳細は [RFC 9112: HTTP/1.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1) を正とする。

## 必須修正

### 1. socket読取を「ヘッダー」と「本文framing」に分離する

`ReadHttpRequest` 内へ処理を追加し続けるのではなく、受信済みバッファとsocketから必要なデータを取り出す小さなreaderを用意する。

最低限、次の操作を持たせる。

- CRLF終端の1行を読む。
- 指定バイト数を正確に読む。
- 受信済みバッファを先に消費し、不足分だけ `recv` する。
- EOF、timeout、受信エラーを区別せずとも、呼出側へ失敗理由を返す。
- ヘッダー、chunk extension、trailer、decoded bodyの各上限を超えたら停止する。

ヘッダー終端検出後のバイト列を捨てないこと。そこには本文または最初のchunkが含まれる場合がある。

### 2. `Content-Length` と `Transfer-Encoding` を排他的に処理する

本文長の決定順序を次にする。

```text
Transfer-Encodingあり:
    Content-Lengthもある場合は400で拒否して接続を閉じる
    transfer codingをカンマ区切り・大文字小文字非区別で解析する
    最終codingがchunkedでなければ400で拒否する
    chunked以外の未対応codingが含まれる場合は501で拒否する
    chunked bodyをdecodeしてrequest.bodyへ格納する

Transfer-Encodingなし:
    Content-Lengthありなら従来どおり固定長で読む
    両方なしなら本文長0として扱う
```

`Transfer-Encoding` と `Content-Length` の同時指定はrequest smugglingの原因になるため、曖昧な優先順位を実装せず400で拒否する。現在のサーバーは1リクエストごとに接続を閉じるが、この拒否経路でも必ず接続を閉じること。

### 3. chunked bodyをRFC 9112に従ってdecodeする

次の順序で処理する。

```text
decodedSize = 0
loop:
    chunk-size行をCRLFまで読む
    最初の「;」より前を16進サイズとして厳密に解析する
    overflow、空サイズ、非16進文字は400
    chunk extensionは解釈せず、長さ上限を確認して無視する

    size == 0 の場合:
        空行までtrailer行を読み、上限を確認して破棄する
        完了

    decodedSize + size が MaxHttpBodyBytes を超える場合は413
    sizeバイトを正確にrequest.bodyへ追加する
    chunk-data直後のCRLFを必須とする
```

注意事項:

- chunk-sizeは分割受信される前提で読む。
- JSONの途中でchunk境界が来ても正しく連結する。
- 16進サイズは大文字・小文字を受け付ける。
- chunk extensionは未知でも無視するが、無制限には読まない。
- trailerは通常ヘッダーへマージせず破棄する。
- terminal chunk後の空行まで必ず消費する。
- 不完全なchunk、CRLF欠落、早期切断を成功扱いしない。

### 4. サイズ制限を分ける

現在はヘッダー読取中にも `MaxHttpBodyBytes` を使っている。次のように用途別の上限を設ける。

- HTTPヘッダー全体: 64 KiB程度
- 1行: 8 KiB程度
- chunk extensionとtrailer合計: 64 KiB程度
- decode後本文: 既存の `MaxHttpBodyBytes`（16 MiB）

上限超過時はJSONパーサーやMCP dispatcherへ渡さず、4xxを返す。

### 5. 応答送信を全量送信ループにする

`SendHttpResponse` は現在 `send` を1回だけ呼んでいるが、Winsockの `send` は全バイト送信を保証しない。`SendAll` helperを追加し、送信済みoffsetを進めながら全量またはエラーまで繰り返す。

これは今回のchunked受信障害とは別だが、大きい `tools/list`、resource、capture応答を安定させるため同じHTTP transport修正に含める。

## 維持する仕様

### `GET /mcp` は405のままでよい

SSEを実装しない場合、現在の `GET /mcp` に対する `405 Method Not Allowed` と `Allow: POST, DELETE` は維持してよい。[MCP 2025-11-25 Streamable HTTP仕様](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports) は、standalone GETに対してSSEを開始するか405を返すことを許可している。

見かけだけのSSE応答や、終了しない空ストリームは追加しない。サーバーからclientへの非同期notificationが必要になった時点で、session lifecycle、切断、再接続、event idを含むSSEを別作業として実装する。

### 対応protocol versionを安易に増やさない

現状の `2025-11-25` と `2025-06-18` 対応は維持する。`server/discover` など新しいprotocolの一部だけを受け付け、実際にはそのversionのlifecycleやschemaへ対応していない状態を作らない。

未対応の `MCP-Protocol-Version` に400を返す現在の方針は維持する。

### セキュリティ境界を弱めない

次を維持する。

- `127.0.0.1` のみでlistenする。
- `Origin` を検証する。
- 全メソッドでBearer認証する。
- token、Authorization値、session idをログへ出さない。
- request timeoutと本文サイズ制限を維持する。
- mutation approval処理は変更しない。

## テスト指示

### 自動テスト

既存の `Tests` と `Scripts/Test*.ps1` の形式に合わせ、HTTP framing専用テストを追加する。候補名は次のとおり。

```text
Tests/McpServerHttpTests.cpp
Scripts/TestMcpServerHttp.ps1
```

少なくとも次を検証する。

1. `Content-Length` 付きinitializeが従来どおり200になる。
2. chunked initializeが200になり、`MCP-Session-Id` が返る。
3. JSONを1文字単位や不規則な位置でchunk分割しても復元できる。
4. chunk extension付き本文を受理できる。
5. terminal chunkと空trailerを処理できる。
6. trailer付き本文を安全に処理できる。
7. chunked `notifications/initialized` が202・空bodyになる。
8. chunked `tools/list` が200になり、tool配列が返る。
9. chunked `tools/call` で読み取り専用のstub toolが呼べる。
10. 不正16進サイズ、size overflow、CRLF欠落、途中EOFを400にする。
11. decode後16 MiB超を413にする。
12. `Content-Length` と `Transfer-Encoding` の同時指定を400にする。
13. 最終codingがchunkedでないrequestを400にする。
14. 未対応transfer codingを501にする。
15. 不正framingではMCP host/dispatcherを一度も呼ばない。
16. `GET /mcp` は405と `Allow` を返す。
17. 認証なしは401、不正Originは403のままである。

可能ならsocket I/Oとframing decoderを分け、framing decoderは純粋なbyte列テストにする。socket integration testでは1回の `send` で全リクエストを送らず、ヘッダー、chunk-size、chunk-data、CRLFを別々に送ってTCP分割を再現する。

### 実クライアント受け入れテスト

修正サーバーを起動し、`LocalMCPChatClient` の対象profileで次を設定する。

```json
{
  "enableStandaloneGetStream": false,
  "bufferHttpRequestBody": false
}
```

Bearer tokenはJSONへ直接書かず、既存どおりWindows資格情報マネージャーの `secretRef` を使う。

次の結果を必須とする。

- `State: Connected`
- `Tools: 36`
- `lookdevpt.get_stats` が成功し、`isError` がfalse
- サーバー側にparse error、pending request、残存sessionがない

その後 `enableStandaloneGetStream: true` でも確認する。SDKが405を正しく扱って接続を継続するならtrueを利用可能とする。SDK側で405が致命扱いになる場合はfalseを既定profileとして維持し、サーバーへ疑似SSEを追加して回避しない。

## Visual Studioプロジェクト要件

新しい `.cpp`、`.h`、build scriptをメイン `.vcxproj` のproject-visible itemとして追加する場合は、`D3D12LookDevPTWinUI.vcxproj` と `D3D12LookDevPTWinUI.vcxproj.filters` を同時に更新する。

- HTTP/MCP実装: `Source Files\\Services` または `Header Files\\Services`
- build/test script: `Build Scripts`
- 各itemはfilters側へちょうど1回だけ登録する。
- 新filterが必要なら一意のGUIDを与える。

完了前に両ファイルをXMLとしてparseし、missing、extra、duplicate、undeclared filterがないことを検証する。

## ビルド・完了条件

1. Visual Studio 2026 / MSVC `v145` の `Debug|x64` でソリューションをビルドする。
2. 既存の `Scripts/Test*.ps1` を実行する。
3. 新しいHTTP framingテストを実行する。
4. `Content-Length` clientとchunked clientの両方でinitializeからDELETEまで確認する。
5. `LocalMCPChatClient` の本文バッファ回避をfalseにして接続、36ツール取得、`get_stats` 成功を確認する。
6. tokenやAuthorization値がsource、test fixture、log、screenshotへ保存されていないことを確認する。
7. `docs/mcp.md` と `docs/mcp.ja.md` に、HTTP/1.1 chunked request対応を追記する。

## 非対象

次は今回の必須範囲に含めない。

- SSE配信
- protocol `2026-07-28` 対応
- OAuth
- remote interfaceでのlisten
- keep-alive/pipelining
- mutation承認方式の変更

## 実装担当者向け短縮指示

```text
Source/McpServer.cpp のHTTP request readerを修正し、RFC 9112準拠の
Transfer-Encoding: chunked request bodyをdecodeしてください。

現状はContent-Lengthしか見ないため、ModelContextProtocol C# SDK 2.0.0の
chunked POST本文を空bodyへ切り詰め、Unexpected end of JSONを返します。

必須:
- Content-Length従来互換
- chunk-size/extension/data/CRLF/terminal chunk/trailerの厳密処理
- decode後16 MiB制限、header/line/trailer個別制限
- CL+TE拒否、unsupported/final-non-chunked拒否
- partial recv対応
- SendHttpResponseのpartial send対応
- GET /mcpの405は維持（SSEは追加しない）
- auth/origin/session/version/approval挙動を維持
- 正常系・異常系socket testを追加
- VS 2026 v145 Debug|x64 buildと全testを通す
- project-visible file追加時はvcxprojとfiltersを完全同期する

受け入れ条件は、LocalMCPChatClient側のbufferHttpRequestBody=falseで
Connected、36 tools、lookdevpt.get_stats成功です。秘密値は出力しないでください。
```
