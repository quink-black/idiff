# tools/idiff-mcp/setup.ps1 -- provision the local venv for the idiff
# MCP server on Windows.  Idempotent: re-running just upgrades deps.
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvDir = Join-Path $ScriptDir ".venv"

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Host "python not found in PATH" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $VenvDir)) {
    Write-Host "Creating venv at $VenvDir"
    python -m venv $VenvDir
}

$PipExe = Join-Path $VenvDir "Scripts\pip.exe"
$PythonExe = Join-Path $VenvDir "Scripts\python.exe"

& $PipExe install --upgrade pip | Out-Null
& $PipExe install -r (Join-Path $ScriptDir "requirements.txt")

Write-Host ""
Write-Host "idiff-mcp ready." -ForegroundColor Green
Write-Host "   venv:  $VenvDir"
Write-Host "   entry: $ScriptDir\idiff_mcp_server.py"
Write-Host ""
Write-Host "Add this to ~/.codebuddy/mcp.json under mcpServers:"
Write-Host ""
$McpJson = @"
  "idiff": {
    "command": "$PythonExe",
    "args": ["$ScriptDir\idiff_mcp_server.py"],
    "env": {}
  }
"@
Write-Host $McpJson
