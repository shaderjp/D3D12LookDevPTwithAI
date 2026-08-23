[CmdletBinding()]
param(
    [string]$PythonExecutable = ''
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pythonArguments = @()
if (-not $PythonExecutable) {
    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($launcher) {
        $PythonExecutable = $launcher.Source
        $pythonArguments += '-3'
    } else {
        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python -or $python.Source -like '*\WindowsApps\python.exe') {
            throw 'Python 3 was not found. Install Python or pass -PythonExecutable.'
        }
        $PythonExecutable = $python.Source
    }
}
$pythonArguments += (Join-Path $repo 'Tests\AnalyzeBenchmarkSequenceTests.py')
& $PythonExecutable @pythonArguments
if ($LASTEXITCODE -ne 0) {
    throw "AnalyzeBenchmarkSequenceTests failed with exit code $LASTEXITCODE."
}
