param(
    [Parameter(Mandatory = $true)]
    [string] $ReadyFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$maximumHeaderBytes = 16 * 1024
$maximumRequestBytes = 256 * 1024
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$listener = $null
$client = $null

function Assert-FixtureCondition {
    param(
        [bool] $Condition,
        [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Read-FixtureLine {
    param(
        [IO.Stream] $Stream,
        [int] $MaximumBytes
    )

    $bytes = [Collections.Generic.List[byte]]::new()
    while ($true) {
        $value = $Stream.ReadByte()
        if ($value -lt 0) {
            throw 'The fixture request ended before a complete line was received.'
        }
        if ($value -eq 13) {
            if ($Stream.ReadByte() -ne 10) {
                throw 'The fixture request used an invalid line ending.'
            }
            return $utf8.GetString($bytes.ToArray())
        }
        if ($value -eq 10) {
            throw 'The fixture request used an invalid line ending.'
        }
        if ($bytes.Count -ge $MaximumBytes) {
            throw 'The fixture request line exceeded its bounded size.'
        }
        $bytes.Add([byte]$value)
    }
}

function Read-FixtureBytes {
    param(
        [IO.Stream] $Stream,
        [int] $Count
    )

    $bytes = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($bytes, $offset, $Count - $offset)
        if ($read -le 0) {
            throw 'The fixture request body ended early.'
        }
        $offset += $read
    }
    return $bytes
}

function Read-FixtureRequestBody {
    param(
        [IO.Stream] $Stream,
        [Collections.Generic.Dictionary[string, string]] $Headers
    )

    if ($Headers.ContainsKey('transfer-encoding')) {
        Assert-FixtureCondition (-not $Headers.ContainsKey('content-length')) `
            'The fixture rejects ambiguous request framing.'
        Assert-FixtureCondition ($Headers['transfer-encoding'] -ceq 'chunked') `
            'The fixture only accepts chunked transfer encoding.'
        $body = [IO.MemoryStream]::new()
        try {
            while ($true) {
                $sizeLine = Read-FixtureLine $Stream 128
                $separator = $sizeLine.IndexOf(';')
                $sizeToken = if ($separator -ge 0) {
                    $sizeLine.Substring(0, $separator)
                }
                else {
                    $sizeLine
                }
                Assert-FixtureCondition ($sizeToken -cmatch '^[0-9A-Fa-f]+$') `
                    'The fixture received an invalid chunk size.'
                $chunkSize = [Convert]::ToInt32($sizeToken, 16)
                if ($chunkSize -eq 0) {
                    $trailerBytes = 0
                    $trailerCount = 0
                    while ($true) {
                        $trailer = Read-FixtureLine $Stream 4096
                        $trailerBytes += $utf8.GetByteCount($trailer) + 2
                        $trailerCount++
                        Assert-FixtureCondition ($trailerBytes -le $maximumHeaderBytes -and
                            $trailerCount -le 32) `
                            'The fixture request trailers exceeded their bounded size.'
                        if ($trailer.Length -eq 0) {
                            break
                        }
                    }
                    break
                }
                Assert-FixtureCondition (($body.Length + $chunkSize) -le $maximumRequestBytes) `
                    'The fixture request body exceeded its bounded size.'
                $chunk = Read-FixtureBytes $Stream $chunkSize
                $body.Write($chunk, 0, $chunk.Length)
                Assert-FixtureCondition ($Stream.ReadByte() -eq 13 -and $Stream.ReadByte() -eq 10) `
                    'The fixture received an invalid chunk terminator.'
            }
            return $body.ToArray()
        }
        finally {
            $body.Dispose()
        }
    }

    Assert-FixtureCondition ($Headers.ContainsKey('content-length')) `
        'The fixture request did not declare a bounded body length.'
    $lengthText = $Headers['content-length']
    Assert-FixtureCondition ($lengthText -cmatch '^\d+$') `
        'The fixture request declared an invalid body length.'
    $length = [Convert]::ToInt32($lengthText, [Globalization.CultureInfo]::InvariantCulture)
    Assert-FixtureCondition ($length -ge 0 -and $length -le $maximumRequestBytes) `
        'The fixture request body exceeded its bounded size.'
    return Read-FixtureBytes $Stream $length
}

try {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(1)
    $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    [IO.File]::WriteAllText(
        $ReadyFile,
        $port.ToString([Globalization.CultureInfo]::InvariantCulture),
        $utf8)

    $accept = $listener.BeginAcceptTcpClient($null, $null)
    try {
        Assert-FixtureCondition ($accept.AsyncWaitHandle.WaitOne([TimeSpan]::FromSeconds(10))) `
            'The fixture timed out waiting for its request.'
        $client = $listener.EndAcceptTcpClient($accept)
    }
    finally {
        $accept.AsyncWaitHandle.Dispose()
    }

    $client.ReceiveTimeout = 10000
    $client.SendTimeout = 10000
    $stream = $client.GetStream()
    $requestLine = Read-FixtureLine $stream 4096
    Assert-FixtureCondition ($requestLine -ceq 'POST /v1/chat/completions HTTP/1.1') `
        'The fixture received an unexpected request target.'

    $headers = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $headerBytes = $utf8.GetByteCount($requestLine) + 2
    while ($true) {
        $line = Read-FixtureLine $stream 4096
        $headerBytes += $utf8.GetByteCount($line) + 2
        Assert-FixtureCondition ($headerBytes -le $maximumHeaderBytes) `
            'The fixture request headers exceeded their bounded size.'
        if ($line.Length -eq 0) {
            break
        }
        $colon = $line.IndexOf(':')
        Assert-FixtureCondition ($colon -gt 0) 'The fixture received an invalid header.'
        $name = $line.Substring(0, $colon)
        $value = $line.Substring($colon + 1).Trim()
        Assert-FixtureCondition (-not $headers.ContainsKey($name)) `
            'The fixture received a duplicate header.'
        $headers.Add($name, $value)
    }

    Assert-FixtureCondition ($headers.ContainsKey('authorization') -and
        $headers['authorization'] -ceq 'Bearer fixture-token') `
        'The fixture received invalid authorization.'

    $bodyBytes = Read-FixtureRequestBody $stream $headers
    $request = ($utf8.GetString($bodyBytes) | ConvertFrom-Json)
    Assert-FixtureCondition ($null -ne $request) 'The fixture received an empty JSON request.'
    Assert-FixtureCondition ($request.model -ceq 'fixture-model') `
        'The fixture received an unexpected model identifier.'
    Assert-FixtureCondition ($request.stream -eq $true -and
        $request.parse_tool_calls -eq $true -and
        $request.parallel_tool_calls -eq $false -and
        $request.tool_choice -ceq 'auto') `
        'The fixture received invalid streaming tool options.'
    Assert-FixtureCondition (@($request.tools).Count -eq 1 -and
        $request.tools[0].type -ceq 'function' -and
        $request.tools[0].function.name -ceq 'lookdev.get_scene_state' -and
        $request.tools[0].function.parameters.type -ceq 'object') `
        'The fixture received an invalid tool definition.'
    Assert-FixtureCondition (@($request.messages).Count -eq 2 -and
        $request.messages[0].role -ceq 'system' -and
        $request.messages[1].role -ceq 'user' -and
        $request.messages[1].content -ceq 'Use the live scene-state tool.') `
        'The fixture received an invalid chat transcript.'

    # This mirrors llama.cpp b10205: tool-call fragments live in delta,
    # finish_reason lives on the choice, then an optional usage-only event
    # precedes the terminal [DONE] marker.
    $sse = @'
data: {"choices":[{"index":0,"delta":{"role":"assistant","content":"Checking ","tool_calls":[{"index":0,"id":"call_","type":"function","function":{"name":"lookdev.get_","arguments":"{\"scope\":\""}}]},"finish_reason":null}]}

data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"scene_","function":{"name":"scene_","arguments":"active_"}}]},"finish_reason":null}]}

data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"state","function":{"name":"state","arguments":"scene\"}"}}]},"finish_reason":null}]}

data: {"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}

data: {"choices":[],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}

data: [DONE]

'@
    $sseBytes = $utf8.GetBytes($sse)
    $responseHeaders =
        "HTTP/1.1 200 OK`r`n" +
        "Content-Type: text/event-stream`r`n" +
        "Content-Length: $($sseBytes.Length)`r`n" +
        "Cache-Control: no-cache`r`n" +
        "Connection: close`r`n`r`n"
    $responseHeaderBytes = $utf8.GetBytes($responseHeaders)
    $stream.Write($responseHeaderBytes, 0, $responseHeaderBytes.Length)
    $stream.Write($sseBytes, 0, $sseBytes.Length)
    $stream.Flush()
}
finally {
    if ($null -ne $client) {
        $client.Dispose()
    }
    if ($null -ne $listener) {
        $listener.Stop()
    }
}
