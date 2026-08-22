[CmdletBinding()]
param(
    [string]$LockPath,
    [switch]$Force,
    [switch]$VerifyMetadataOnly
)

$ErrorActionPreference = 'Stop'
$scriptDirectory = [IO.Path]::GetFullPath($PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($LockPath)) {
    $packagedLock = Join-Path $scriptDirectory 'suite.lock.json'
    $repositoryLock = Join-Path (Split-Path -Parent $scriptDirectory) 'suite.lock.json'
    $LockPath = if (Test-Path -LiteralPath $packagedLock -PathType Leaf) { $packagedLock } else { $repositoryLock }
}
$LockPath = [IO.Path]::GetFullPath($LockPath)
$lock = Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
$runtimeVersion = [string]$lock.runtimes.windowsAppSdk
$installer = $lock.runtimes.windowsAppRuntimeInstaller
if ($runtimeVersion -notmatch '^(?<major>\d+)\.(?<minor>\d+)\.' -or $null -eq $installer) {
    throw 'suite.lock.json does not contain a valid Windows App Runtime prerequisite.'
}
$runtimeMajor = $Matches.major
$runtimeMinor = $Matches.minor
$uri = [uri][string]$installer.uri
$expectedHash = ([string]$installer.sha256).ToLowerInvariant()
$arguments = @($installer.silentArguments | ForEach-Object { [string]$_ })
if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'aka.ms' -or
    $uri.AbsolutePath -notmatch '^/windowsappsdk/[0-9.]+/[0-9.]+/windowsappruntimeinstall-x64\.exe$') {
    throw "Windows App Runtime installer URI is not on the approved Microsoft endpoint: $uri"
}
if ($expectedHash -notmatch '^[0-9a-f]{64}$') { throw 'Windows App Runtime installer SHA-256 is invalid.' }
if ($arguments.Count -ne 1 -or $arguments[0] -ne '--quiet') { throw 'Only the documented --quiet installer argument is allowed.' }
if ([string]$installer.architecture -ne 'x64') { throw 'This suite only permits the x64 Windows App Runtime installer.' }

$packageName = "Microsoft.WindowsAppRuntime.$runtimeMajor.$runtimeMinor"
if ($VerifyMetadataOnly) {
    [pscustomobject]@{ Valid = $true; PackageName = $packageName; Version = $runtimeVersion; Uri = $uri; Sha256 = $expectedHash }
    return
}

$installed = @(Get-AppxPackage -Name $packageName -ErrorAction SilentlyContinue)
if (-not $Force -and $installed.Count -gt 0) {
    Write-Host "Windows App Runtime $runtimeVersion is already available for the current user."
    return
}

$downloadDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'D3D12LookDevPTwithAI\RuntimeInstaller'
New-Item -ItemType Directory -Path $downloadDirectory -Force | Out-Null
$downloadPath = Join-Path $downloadDirectory ("{0}-{1}" -f ([guid]::NewGuid().ToString('N')), [string]$installer.fileName)
try {
    Write-Host "Downloading the Microsoft Windows App Runtime $runtimeVersion installer..."
    Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $downloadPath
    $actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) { throw "Windows App Runtime installer hash mismatch: $actualHash" }
    $signature = Get-AuthenticodeSignature -LiteralPath $downloadPath
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
        throw "Windows App Runtime installer signature is not valid Microsoft code: $($signature.Status)"
    }

    Write-Host 'Installing the Microsoft runtime. Without elevation it is registered for the current user.'
    $process = Start-Process -FilePath $downloadPath -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Windows App Runtime installer failed with exit code $($process.ExitCode)." }
}
finally {
    if (Test-Path -LiteralPath $downloadPath -PathType Leaf) { [IO.File]::Delete($downloadPath) }
}

$installed = @(Get-AppxPackage -Name $packageName -ErrorAction SilentlyContinue)
if ($installed.Count -eq 0) { throw "Windows App Runtime package $packageName was not found after installation." }
Write-Host "Windows App Runtime $runtimeVersion is ready."
