param(
    [ValidatePattern('^[a-z_][a-z0-9_-]{0,31}$')]
    [string]$User = 'root',
    [ValidateRange(1, 65535)]
    [int]$Port = 22,
    [string]$ServerIp = '121.41.86.40',
    [switch]$Install,
    [switch]$UpdateConfig
)
$ErrorActionPreference = 'Stop'
# Rebuild the explicit package, preserving the separate private AI settings.
& python (Join-Path $PSScriptRoot 'configure.py') '--server-ip' $ServerIp
if ($LASTEXITCODE -ne 0) { throw 'AI configuration/package failed; upload stopped.' }
$sshTarget = "${User}@${ServerIp}"
$sshArgs = @('-p', "$Port", '-o', 'ConnectTimeout=15', $sshTarget)
& ssh.exe @sshArgs 'install -d -m 700 -- .stubuddy-ai-stage'
if ($LASTEXITCODE -ne 0) { throw 'SSH failed; upload stopped.' }
& scp.exe '-P' "$Port" (Join-Path $PSScriptRoot '.deploy.tar.gz') "${sshTarget}:.stubuddy-ai-stage/upload.tar.gz"
if ($LASTEXITCODE -ne 0) { throw 'Private AI package upload failed.' }
& ssh.exe @sshArgs 'tar -xzf .stubuddy-ai-stage/upload.tar.gz -C .stubuddy-ai-stage'
if ($LASTEXITCODE -ne 0) { throw 'AI extraction failed.' }
$remotePython = if ($User -eq 'root') { 'python3' } else { 'sudo python3' }
$action = if ($Install) { '--install' } else { '' }
if ($UpdateConfig) { $action += ' --replace-config' }
& ssh.exe '-t' @sshArgs "$remotePython .stubuddy-ai-stage/install_ecs.py $action"
if ($LASTEXITCODE -ne 0) { throw 'AI check/install failed; preserve STOP output.' }
if ($Install) {
    Write-Host 'AI installed. Run the WSS/provider smoke test before compiling/flashing.'
} else {
    Write-Host 'CHECK PASS; no system service or Nginx configuration changed.'
}
