Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-NvidiaDependencyManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $manifest = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 1 -or !$manifest.profiles -or !$manifest.components) {
        throw "Unsupported or incomplete NVIDIA dependency manifest: $resolved"
    }
    $ids = @($manifest.components | ForEach-Object { [string]$_.id })
    if (($ids | Sort-Object -Unique).Count -ne $ids.Count) {
        throw 'The NVIDIA dependency manifest contains duplicate component IDs.'
    }
    $runtimeDestinations = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($component in $manifest.components) {
        if ([string]$component.id -notmatch '^[a-z0-9-]+$' -or
            [string]$component.rootProperty -notmatch '^[A-Za-z][A-Za-z0-9]+$' -or
            [string]$component.revision -notmatch '^[0-9a-f]{40}$') {
            throw "Invalid NVIDIA component identity metadata: $($component.id)"
        }
        $relativePaths = @([string]$component.defaultRoot, [string]$component.submodulePath)
        $relativePaths += @($component.developmentFiles)
        $relativePaths += @($component.buildOutputs)
        $relativePaths += @($component.licenseFiles)
        foreach ($runtime in @($component.runtimeFiles)) {
            $relativePaths += [string]$runtime.path
            $relativePaths += [string]$runtime.destination
            if (!$runtimeDestinations.Add([string]$runtime.destination)) {
                throw "Duplicate NVIDIA runtime destination: $($runtime.destination)"
            }
        }
        foreach ($relative in $relativePaths) {
            $segments = ([string]$relative) -split '[\\/]'
            if ([string]::IsNullOrWhiteSpace([string]$relative) -or
                [IO.Path]::IsPathRooted([string]$relative) -or
                $segments -contains '..' -or $segments -contains '.') {
                throw "Unsafe NVIDIA manifest relative path: $relative"
            }
        }
    }
    return $manifest
}

function Get-NvidiaProfile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)][string]$Name
    )

    $property = $Manifest.profiles.PSObject.Properties[$Name]
    if (!$property) {
        throw "Unknown NVIDIA profile '$Name'."
    }
    return $property.Value
}

function Get-NvidiaComponentRoots {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [hashtable]$Overrides = @{}
    )

    $roots = @{}
    foreach ($component in $Manifest.components) {
        $id = [string]$component.id
        $value = if ($Overrides.ContainsKey($id) -and $Overrides[$id]) {
            [string]$Overrides[$id]
        } else {
            Join-Path $RepositoryRoot ([string]$component.defaultRoot)
        }
        $roots[$id] = [IO.Path]::GetFullPath($value).TrimEnd([char[]]@('\', '/'))
    }
    return $roots
}

function Test-NvidiaComponentEnabled {
    param($Component, $Profile)
    $key = [string]$Component.enableWhen
    $property = $Profile.PSObject.Properties[$key]
    return $property -and [bool]$property.Value
}

function Get-NvidiaGpuInfo {
    [CmdletBinding()]
    param()

    $command = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if (!$command) {
        return [pscustomobject]@{ Available = $false; Gpus = @(); Message = 'nvidia-smi.exe was not found.' }
    }
    try {
        $lines = @(& $command.Source --query-gpu=name,driver_version,compute_cap --format=csv,noheader 2>$null)
        if ($LASTEXITCODE -ne 0 -or $lines.Count -eq 0) {
            throw 'nvidia-smi returned no GPU records.'
        }
        return [pscustomobject]@{ Available = $true; Gpus = $lines; Message = "$($lines.Count) NVIDIA GPU(s) detected." }
    } catch {
        return [pscustomobject]@{ Available = $false; Gpus = @(); Message = $_.Exception.Message }
    }
}

function Get-NvidiaDependencyReport {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][hashtable]$Roots,
        [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
        [ValidateSet('Prepare', 'Complete')][string]$Phase = 'Prepare',
        [switch]$LicenseAccepted
    )

    $checks = [Collections.Generic.List[object]]::new()
    $add = {
        param($status, $component, $kind, $message)
        $checks.Add([pscustomobject]@{ Status = $status; Component = $component; Kind = $kind; Message = $message })
    }
    $gpu = Get-NvidiaGpuInfo
    $gpuStatus = if ($gpu.Available) { 'OK' } elseif ([bool]$Profile.requireGpu) { 'FAIL' } else { 'WARN' }
    & $add $gpuStatus 'driver' 'gpu' $gpu.Message

    $ngxName = [string]$Manifest.ngxApplicationIdEnvironmentVariable
    $ngxValue = [Environment]::GetEnvironmentVariable($ngxName)
    $ngxPresent = ![string]::IsNullOrWhiteSpace($ngxValue)
    $ngxValid = $ngxPresent -and $ngxValue -match '^\d+$'
    if ($ngxValid) {
        & $add 'OK' 'ngx' 'applicationId' "$ngxName is set to a decimal value (value intentionally hidden)."
    } elseif ([bool]$Profile.requireNgxApplicationId) {
        & $add 'FAIL' 'ngx' 'applicationId' "$ngxName must contain the NVIDIA-issued decimal application ID."
    } else {
        & $add 'WARN' 'ngx' 'applicationId' "$ngxName is not set; DLSS runtime initialization will remain unavailable."
    }

    if ([bool]$Profile.requireLicenseAcceptance -and !$LicenseAccepted) {
        & $add 'FAIL' 'license' 'acceptance' 'Release staging requires explicit -AcceptNvidiaLicense confirmation.'
    }

    foreach ($component in $Manifest.components) {
        if (!(Test-NvidiaComponentEnabled $component $Profile)) { continue }
        $root = [string]$Roots[[string]$component.id]
        if (!(Test-Path -LiteralPath $root -PathType Container)) {
            & $add 'FAIL' $component.id 'root' "$($component.displayName) root is missing: $root"
            continue
        }
        foreach ($relative in @($component.developmentFiles)) {
            $path = Join-Path $root ([string]$relative)
            $status = if (Test-Path -LiteralPath $path -PathType Leaf) { 'OK' } else { 'FAIL' }
            & $add $status $component.id 'development' ([string]$relative)
        }
        foreach ($relative in @($component.licenseFiles)) {
            $path = Join-Path $root ([string]$relative)
            $status = if (Test-Path -LiteralPath $path -PathType Leaf) { 'OK' } else { 'FAIL' }
            & $add $status $component.id 'license' ([string]$relative)
        }
        $git = Get-Command git.exe -ErrorAction SilentlyContinue
        if ($git) {
            $actualRevision = [string](& $git.Source -C $root rev-parse HEAD 2>$null | Select-Object -First 1)
            if ($LASTEXITCODE -eq 0 -and $actualRevision) {
                $actualRevision = $actualRevision.Trim()
                $status = if ($actualRevision -ieq [string]$component.revision) { 'OK' } else { 'FAIL' }
                & $add $status $component.id 'revision' "expected $($component.revision); actual $actualRevision"
                $submoduleStatus = @(& $git.Source -C $root submodule status --recursive 2>$null)
                $invalidSubmodules = @($submoduleStatus | Where-Object { $_ -match '^[+\-U]' })
                $submoduleCheckStatus = if ($LASTEXITCODE -eq 0 -and $invalidSubmodules.Count -eq 0) { 'OK' } else { 'FAIL' }
                $submoduleMessage = if ($invalidSubmodules.Count -eq 0) {
                    'Nested submodules match the component revision.'
                } else {
                    "Nested submodule mismatch or missing checkout: $($invalidSubmodules -join '; ')"
                }
                & $add $submoduleCheckStatus $component.id 'nestedRevisions' $submoduleMessage
            } else {
                & $add 'WARN' $component.id 'revision' 'External SDK root is not a Git checkout; file validation is used instead.'
            }
        }
        if ($Phase -eq 'Complete') {
            foreach ($relativeTemplate in @($component.buildOutputs)) {
                $relative = ([string]$relativeTemplate).Replace('{configuration}', $Configuration)
                $status = if (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf) { 'OK' } else { 'FAIL' }
                & $add $status $component.id 'buildOutput' $relative
            }
            foreach ($runtime in @($component.runtimeFiles)) {
                $exists = Test-Path -LiteralPath (Join-Path $root ([string]$runtime.path)) -PathType Leaf
                $status = if ($exists) { 'OK' } elseif ([bool]$runtime.required) { 'FAIL' } else { 'WARN' }
                & $add $status $component.id 'runtime' ([string]$runtime.path)
            }
        }
    }

    $failures = @($checks | Where-Object Status -eq 'FAIL').Count
    $warnings = @($checks | Where-Object Status -eq 'WARN').Count
    return [pscustomobject]@{
        SchemaVersion = 1
        Phase = $Phase
        Configuration = $Configuration
        NgxApplicationIdEnvironmentVariable = $ngxName
        NgxApplicationIdPresent = $ngxValid
        Gpu = $gpu
        Failures = $failures
        Warnings = $warnings
        Checks = $checks
    }
}

function Assert-NvidiaSubmodulesSafe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][hashtable]$Roots
    )

    $git = (Get-Command git.exe -ErrorAction Stop).Source
    foreach ($component in $Manifest.components) {
        if (!(Test-NvidiaComponentEnabled $component $Profile)) { continue }
        $expected = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot ([string]$component.defaultRoot))).TrimEnd([char[]]@('\', '/'))
        if ([string]$Roots[[string]$component.id] -ine $expected) { continue }
        if (Test-Path -LiteralPath $expected -PathType Container) {
            $dirty = @(& $git -C $expected status --porcelain --untracked-files=no 2>$null)
            if ($LASTEXITCODE -eq 0 -and $dirty.Count -gt 0) {
                throw "Refusing to update dirty NVIDIA submodule '$($component.submodulePath)'. Preserve or clean its changes first."
            }
        }
    }
}

function Initialize-NvidiaSubmodules {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][hashtable]$Roots
    )

    Assert-NvidiaSubmodulesSafe -Manifest $Manifest -Profile $Profile -RepositoryRoot $RepositoryRoot -Roots $Roots
    $paths = @($Manifest.components | Where-Object {
            if (!(Test-NvidiaComponentEnabled $_ $Profile)) { return $false }
            $expected = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot ([string]$_.defaultRoot))).TrimEnd([char[]]@('\', '/'))
            return [string]$Roots[[string]$_.id] -ieq $expected
        } | ForEach-Object { [string]$_.submodulePath })
    if ($paths.Count -gt 0) {
        $git = (Get-Command git.exe -ErrorAction Stop).Source
        & $git -C $RepositoryRoot submodule update --init --recursive -- @paths
        if ($LASTEXITCODE -ne 0) { throw 'NVIDIA submodule initialization failed.' }
    }
}

function Get-NvidiaMsBuildProperties {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][hashtable]$Roots
    )

    $dlss = ([bool]$Profile.dlss).ToString().ToLowerInvariant()
    $nrd = ([bool]$Profile.nrd).ToString().ToLowerInvariant()
    $rtxdi = ([bool]$Profile.rtxdi).ToString().ToLowerInvariant()
    $properties = @(
        "/p:EnableDLSS=$dlss",
        "/p:EnableNRD=$nrd",
        "/p:EnableRTXDI=$rtxdi"
    )
    if ($Profile.PSObject.Properties['windowsAppSdkSelfContained'] -and [bool]$Profile.windowsAppSdkSelfContained) {
        $properties += '/p:WindowsAppSDKSelfContained=true'
    }
    foreach ($component in $Manifest.components) {
        $properties += "/p:$($component.rootProperty)=$($Roots[[string]$component.id])"
    }
    return $properties
}

function Publish-NvidiaRuntime {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][hashtable]$Roots,
        [Parameter(Mandatory)][string]$DestinationRoot
    )

    $destinationRootFull = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd([char[]]@('\', '/'))
    $destinationPrefix = $destinationRootFull + [IO.Path]::DirectorySeparatorChar
    $records = [Collections.Generic.List[object]]::new()
    foreach ($component in $Manifest.components) {
        if (!(Test-NvidiaComponentEnabled $component $Profile)) { continue }
        $root = [string]$Roots[[string]$component.id]
        foreach ($runtime in @($component.runtimeFiles)) {
            $source = Join-Path $root ([string]$runtime.path)
            if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
                if ([bool]$runtime.required) { throw "Required NVIDIA runtime is missing: $($runtime.path)" }
                continue
            }
            $destination = Join-Path $DestinationRoot ([string]$runtime.destination)
            $destination = [IO.Path]::GetFullPath($destination)
            if (!$destination.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "NVIDIA runtime destination escaped the staging root: $($runtime.destination)"
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination -Force
            $records.Add([pscustomobject]@{ Component = $component.id; Kind = 'runtime'; Path = [string]$runtime.destination })
        }
        foreach ($license in @($component.licenseFiles)) {
            $source = Join-Path $root ([string]$license)
            if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required NVIDIA license is missing: $source" }
            $name = "$($component.id)-$([IO.Path]::GetFileName([string]$license))"
            $relative = Join-Path 'Licenses\NVIDIA' $name
            $destination = Join-Path $DestinationRoot $relative
            $destination = [IO.Path]::GetFullPath($destination)
            if (!$destination.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "NVIDIA license destination escaped the staging root: $relative"
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination -Force
            $records.Add([pscustomobject]@{ Component = $component.id; Kind = 'license'; Path = $relative.Replace('\', '/') })
        }
    }
    return $records
}

Export-ModuleMember -Function Import-NvidiaDependencyManifest, Get-NvidiaProfile, Get-NvidiaComponentRoots, Get-NvidiaDependencyReport, Assert-NvidiaSubmodulesSafe, Initialize-NvidiaSubmodules, Get-NvidiaMsBuildProperties, Publish-NvidiaRuntime
