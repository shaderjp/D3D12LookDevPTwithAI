[CmdletBinding(SupportsShouldProcess, ConfirmImpact='High')]
param(
    [string]$InstallDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs\D3D12LookDevSuite'),
    [switch]$RemoveLocalApplicationData
)

$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory)
$programRoot = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs')).TrimEnd('\') + '\'
if (-not $install.StartsWith($programRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($install) -ne 'D3D12LookDevSuite') {
    throw "Refusing to remove a directory outside the expected per-user suite target: $install"
}
if ((Test-Path -LiteralPath $install) -and $PSCmdlet.ShouldProcess($install, 'Remove portable application files')) {
    [IO.Directory]::Delete($install, $true)
    Write-Host "Removed application files: $install"
}
if ($RemoveLocalApplicationData) {
    $dataTargets = @(
        (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LocalMCPChatClient'),
        (Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'D3D12LookDevPTWinUI')
    )
    foreach ($target in $dataTargets) {
        $resolved = [IO.Path]::GetFullPath($target)
        if ((Test-Path -LiteralPath $resolved) -and $PSCmdlet.ShouldProcess($resolved, 'Remove settings, artifacts, and local history')) {
            [IO.Directory]::Delete($resolved, $true)
            Write-Host "Removed local data (not recoverable from the suite): $resolved"
        }
    }
} else {
    Write-Host 'Settings, conversation history, artifacts, approval rules, and Windows credentials were retained.'
}
