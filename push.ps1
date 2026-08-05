$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\bin;C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\cmd;C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\mingw64\bin;C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\mingw64\libexec\git-core;C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\usr\bin;" + $env:PATH
$env:GIT_EXEC_PATH = "C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\mingw64\libexec\git-core"
$env:HTTP_PROXY = ""
$env:HTTPS_PROXY = ""
$env:ALL_PROXY = ""
$env:GIT_HTTP_PROXY = ""
$env:GIT_HTTPS_PROXY = ""
$env:GIT_TERMINAL_PROMPT = "0"

git commit -m "Build 198"

$inputStr = "protocol=https`nhost=github.com`n`n"
$gcmPath = "C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2\mingw64\bin\git-credential-manager.exe"
$creds = $inputStr | & $gcmPath get

$username = ""
$password = ""
foreach ($line in ($creds -split "`r`n")) {
    if ($line.StartsWith("username=")) { $username = $line.Substring(9) }
    if ($line.StartsWith("password=")) { $password = $line.Substring(9) }
}

if ([string]::IsNullOrEmpty($username) -or [string]::IsNullOrEmpty($password)) {
    Write-Error "Failed to retrieve credentials."
    exit 1
}

$authStr = "$($username):$($password)"
$bytes = [System.Text.Encoding]::UTF8.GetBytes($authStr)
$base64 = [System.Convert]::ToBase64String($bytes)
$header = "AUTHORIZATION: basic $base64"

# Push master
git -c http.sslBackend=openssl -c credential.helper= -c core.askPass= -c "http.extraHeader=$header" push origin master

if ($LASTEXITCODE -eq 0) {
    # Tag and push nightly
    git tag -f nightly
    git -c http.sslBackend=openssl -c credential.helper= -c core.askPass= -c "http.extraHeader=$header" push --force origin refs/tags/nightly
} else {
    Write-Error "Push to master failed."
}

# Cleanup
$username = $null
$password = $null
$authStr = $null
$bytes = $null
$base64 = $null
$header = $null
