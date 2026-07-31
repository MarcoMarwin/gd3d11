$workspaceRoot = "C:\Users\winkler.WS\Documents\Antigravity\Projekte"
$gitBase = "$workspaceRoot\tools\PortableGit-2.55.0.2"

$env:PATH = "$gitBase\bin;$gitBase\cmd;$gitBase\mingw64\bin;$gitBase\mingw64\libexec\git-core;$gitBase\usr\bin;" + $env:PATH
$env:GIT_EXEC_PATH = "$gitBase\mingw64\libexec\git-core"
$env:HTTP_PROXY = ""
$env:HTTPS_PROXY = ""
$env:ALL_PROXY = ""
$env:GIT_HTTP_PROXY = ""
$env:GIT_HTTPS_PROXY = ""
$env:GIT_TERMINAL_PROMPT = "0"

$credInput = "protocol=https`nhost=github.com`n`n"
$credOutput = $credInput | & "$gitBase\mingw64\bin\git-credential-manager.exe" get

$u = ""
$p = ""
foreach ($line in $credOutput) {
    if ($line -match "^username=(.*)") { $u = $matches[1].Trim() }
    if ($line -match "^password=(.*)") { $p = $matches[1].Trim() }
}

if ($u -and $p) {
    $raw = "$u`:$p"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($raw)
    $b64 = [Convert]::ToBase64String($bytes)

    git add -A
    git commit -m "Build 183 (Regulaerer Push): F11-Menue aufgeraeumt, Preset-Entkopplung, kompakte UI-Elemente, doppelte Rain-Rendering-Zeile entfernt"
    
    git -c http.sslBackend=openssl -c credential.helper= -c core.askPass= -c "http.extraHeader=AUTHORIZATION: basic $b64" push origin master

    if ($LASTEXITCODE -eq 0) {
        git tag -f nightly
        git -c http.sslBackend=openssl -c credential.helper= -c core.askPass= -c "http.extraHeader=AUTHORIZATION: basic $b64" push --force origin refs/tags/nightly
        Write-Host "Push successful"
    } else {
        Write-Host "Push failed"
    }

    $u = $null
    $p = $null
    $raw = $null
    $bytes = $null
    $b64 = $null
} else {
    Write-Host "Credential Manager returned no output or mismatch"
}
