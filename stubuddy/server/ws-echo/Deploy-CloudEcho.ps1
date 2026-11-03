param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z_][a-z0-9_-]{0,31}$')]
    [string]$User,
    [ValidateRange(1, 65535)]
    [int]$Port = 22,
    [string]$ServerIp = '121.41.86.40',
    [string]$Email,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$sshProgram = (Get-Command ssh.exe -CommandType Application -ErrorAction Stop).Source
$scpProgram = (Get-Command scp.exe -CommandType Application -ErrorAction Stop).Source
if ($Install -and -not $Email) {
    $Email = Read-Host 'ACME account contact email (not a password)'
}
$configureArgs = @((Join-Path $PSScriptRoot 'configure_cloud.py'), '--server-ip', $ServerIp)
if ($Email) { $configureArgs += @('--email', $Email) }
& python @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'Cloud configuration failed; upload stopped.' }

# Python validates the IP before it is used as an SSH destination.
$sshTarget = "${User}@${ServerIp}"
$sshArgs = @('-p', "$Port", '-o', 'ConnectTimeout=15', $sshTarget)
& $sshProgram @sshArgs 'install -d -m 700 -- .stubuddy-wss-stage'
if ($LASTEXITCODE -ne 0) { throw 'SSH login failed; upload stopped.' }

$archive = Join-Path $PSScriptRoot '.cloud-deploy.tar.gz'
& $scpProgram '-P' "$Port" $archive "${sshTarget}:.stubuddy-wss-stage/upload.tar.gz"
if ($LASTEXITCODE -ne 0) { throw 'Private archive upload failed.' }
& $sshProgram @sshArgs 'tar -xzf .stubuddy-wss-stage/upload.tar.gz -C .stubuddy-wss-stage'
if ($LASTEXITCODE -ne 0) { throw 'Remote archive extraction failed.' }

$pythonCommand = if ($User -eq 'root') { 'python3' } else { 'sudo python3' }
& $sshProgram '-t' @sshArgs "$pythonCommand .stubuddy-wss-stage/install_ecs.py --check"
if ($LASTEXITCODE -ne 0) { throw 'ECS preflight failed; existing services were not replaced.' }
if ($Install) {
    & $sshProgram '-t' @sshArgs "$pythonCommand .stubuddy-wss-stage/install_ecs.py --install"
    if ($LASTEXITCODE -ne 0) { throw 'ECS installation stopped; preserve the final error output.' }
    & python (Join-Path $PSScriptRoot 'smoke_test.py') '--cloud'
    if ($LASTEXITCODE -ne 0) { throw 'Public WSS verification failed; do not flash yet.' }
} else {
    Write-Host 'CHECK PASS. After opening public TCP 80/443, run again with -Install.'
}
