[CmdletBinding()]
param(
    [Parameter(Mandatory)][uri]$PackageUri,
    [string]$Sha256,
    [string]$InstallDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs\D3D12LookDevSuite'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory)
$programRoot = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs')).TrimEnd('\') + '\'
if (-not $install.StartsWith($programRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallDirectory must be below the current user's LocalAppData\Programs directory: $install"
}
if ((Test-Path -LiteralPath $install) -and -not $Force) {
    throw "InstallDirectory already exists. Re-run with -Force to replace this suite installation: $install"
}

$temporaryRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('lookdev-suite-install-' + [Guid]::NewGuid().ToString('N'))))
$archive = Join-Path $temporaryRoot 'suite.zip'
$staging = Join-Path $temporaryRoot 'expanded'
[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    if ($PackageUri.IsFile) {
        Copy-Item -LiteralPath $PackageUri.LocalPath -Destination $archive
    } else {
        Invoke-WebRequest -Uri $PackageUri -OutFile $archive -UseBasicParsing
    }
    if (-not [string]::IsNullOrWhiteSpace($Sha256)) {
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($actual -ne $Sha256) { throw "Portable suite SHA-256 mismatch. Expected $Sha256, received $actual." }
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $totalBytes = 0L
        $entryCount = 0
        foreach ($entry in $zip.Entries) {
            ++$entryCount
            if ($entryCount -gt 20000) { throw 'Portable suite exceeds the archive entry-count limit.' }
            $relative = $entry.FullName.Replace('/','\').TrimEnd('\')
            if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
                $relative -match '(^|\\)\.\.(\\|$)') { throw "Unsafe archive path: $relative" }
            foreach ($segment in $relative.Split('\')) {
                if ([string]::IsNullOrWhiteSpace($segment) -or $segment -eq '.' -or
                    $segment.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
                    throw "Unsafe archive path segment: $relative"
                }
            }
            if (-not $seen.Add($relative)) { throw "Duplicate archive path: $relative" }
            $totalBytes += $entry.Length
            if ($totalBytes -gt 16GB) { throw 'Portable suite exceeds the expanded-size limit.' }
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixType -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400) -ne 0) {
                throw "Links/reparse entries are not allowed: $relative"
            }
            $target = [IO.Path]::GetFullPath((Join-Path $staging $relative))
            $stagingPrefix = [IO.Path]::GetFullPath($staging).TrimEnd('\') + '\'
            if (-not $target.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive path escapes staging: $relative"
            }
            if ($entry.Name.Length -eq 0) { [IO.Directory]::CreateDirectory($target) | Out-Null; continue }
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
        }
    }
    finally { $zip.Dispose() }
    $manifest = Get-ChildItem -LiteralPath $staging -Filter suite-manifest.json -File -Recurse
    if ($manifest.Count -ne 1) { throw 'Portable suite must contain exactly one suite-manifest.json.' }
    $suiteRoot = Split-Path -Parent $manifest[0].FullName
    $manifestJson = Get-Content -LiteralPath $manifest[0].FullName -Raw | ConvertFrom-Json
    if ($manifestJson.schemaVersion -ne 1 -or $manifestJson.platform -ne 'windows-11-x64') {
        throw 'Unsupported portable suite manifest.'
    }
    $manifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $manifestJson.files) {
        $relative = ([string]$record.path).Replace('/','\')
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)') { throw "Unsafe manifest path: $relative" }
        if (-not $manifestPaths.Add($relative)) { throw "Duplicate manifest path: $relative" }
        if ([long]$record.size -lt 0 -or [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Invalid manifest metadata: $relative"
        }
        $file = [IO.Path]::GetFullPath((Join-Path $suiteRoot $relative))
        $rootPrefix = [IO.Path]::GetFullPath($suiteRoot).TrimEnd('\') + '\'
        if (-not $file.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Manifest path escapes suite root: $relative" }
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Manifest file is missing: $relative" }
        if ((Get-Item -LiteralPath $file).Length -ne [long]$record.size) { throw "Manifest size mismatch: $relative" }
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) { throw "Manifest hash mismatch: $relative" }
    }
    foreach ($file in Get-ChildItem -LiteralPath $suiteRoot -File -Recurse) {
        $relative = [IO.Path]::GetRelativePath($suiteRoot, $file.FullName)
        if ($relative -ne 'suite-manifest.json' -and -not $manifestPaths.Contains($relative)) {
            throw "Unmanifested suite file: $relative"
        }
        if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse file is not allowed: $relative"
        }
    }
    $parent = Split-Path -Parent $install
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    if (Test-Path -LiteralPath $install) {
        $backup = "$install.previous-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))"
        Move-Item -LiteralPath $install -Destination $backup
        Write-Warning "Previous installation was retained at $backup."
    }
    Move-Item -LiteralPath $suiteRoot -Destination $install
}
finally {
    $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $temporaryRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($temporaryRoot) -notlike 'lookdev-suite-install-*') {
        throw "Refusing cleanup outside the installer temp root: $temporaryRoot"
    }
    if ([IO.Directory]::Exists($temporaryRoot)) { [IO.Directory]::Delete($temporaryRoot, $true) }
}
Write-Host "Installed without administrator privileges: $install"
Write-Host "Run Launch-LookDevSuite.ps1 and complete the one-time pairing wizard in both applications."
