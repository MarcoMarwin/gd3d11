$ErrorActionPreference = "Stop"

# 1. Update changelog
.\update_changelog.ps1
Remove-Item update_changelog.ps1

# 2. Setup PortableGit Environment
$gitDir = "C:\Users\winkler.WS\Documents\Antigravity\Projekte\tools\PortableGit-2.55.0.2"
$env:PATH = "$gitDir\bin;$gitDir\cmd;$gitDir\mingw64\bin;$gitDir\mingw64\libexec\git-core;$gitDir\usr\bin;" + $env:PATH
$env:GIT_EXEC_PATH = "$gitDir\mingw64\libexec\git-core"
$env:HTTP_PROXY = ""
$env:HTTPS_PROXY = ""
$env:ALL_PROXY = ""
$env:GIT_HTTP_PROXY = ""
$env:GIT_HTTPS_PROXY = ""
$env:GIT_TERMINAL_PROMPT = "0"

# 3. Commit
git add -A
git commit -m "Build 186: Clean up editor widgets, LoadCustomZENResources, wet ground normal"

# 4. Get Credentials
$credInput = "protocol=https`nhost=github.com`n`n"
$credOutput = $credInput | & "$gitDir\mingw64\bin\git-credential-manager.exe" get

$username = ""
$password = ""
foreach ($line in ($credOutput -split "`n")) {
    if ($line -match "^username=(.*)") { $username = $matches[1].Trim() }
    if ($line -match "^password=(.*)") { $password = $matches[1].Trim() }
}

if (-not $password) {
    Write-Error "Failed to retrieve password from credential manager."
    exit 1
}

# 5. Base64 encoding
$authString = "${username}:${password}"
$authBytes = [System.Text.Encoding]::UTF8.GetBytes($authString)
$authBase64 = [Convert]::ToBase64String($authBytes)
$authHeader = "AUTHORIZATION: basic $authBase64"

$gitArgs = @("-c", "http.sslBackend=openssl", "-c", "credential.helper=", "-c", "core.askPass=", "-c", "http.extraHeader=$authHeader")

# 6. Push origin master
Write-Host "Pushing to origin master..."
& git @gitArgs push origin master
$pushResult = $LASTEXITCODE

if ($pushResult -ne 0) {
    Write-Error "Push to master failed."
    exit 1
}

# 7. Tag and push tag
Write-Host "Tagging nightly..."
& git tag -f nightly
Write-Host "Pushing tag nightly..."
& git @gitArgs push --force origin refs/tags/nightly

# Cleanup variables
$password = $null
$authString = $null
$authBytes = $null
$authBase64 = $null
$authHeader = $null
$gitArgs = $null

Write-Host "Push complete!"
