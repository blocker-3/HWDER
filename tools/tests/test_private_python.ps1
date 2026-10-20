# Synthetic ZIPs/bytes only. Never runs the interpreter payload or uses network.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../setup_ui/install_private_python.ps1')
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../build/mt'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$case = Join-Path $root ([Guid]::NewGuid().ToString('N').Substring(0,8))
[IO.Directory]::CreateDirectory($case) | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Require($Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Make-Zip([string]$Name, $Entries) {
    $path = Join-Path $case $Name
    $stream = [IO.File]::Open($path,[IO.FileMode]::CreateNew)
    $zip = New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
    try {
        foreach ($pair in $Entries) {
            $entry=$zip.CreateEntry($pair[0])
            if ($pair.Count -gt 2) { $entry.ExternalAttributes=$pair[2] }
            $out=$entry.Open()
            try { $bytes=[Text.Encoding]::UTF8.GetBytes($pair[1]);$out.Write($bytes,0,$bytes.Length) } finally { $out.Dispose() }
        }
    } finally { $zip.Dispose();$stream.Dispose() }
    return $path
}
function Rejects([scriptblock]$Action) {
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    Require $rejected 'Expected a fail-closed rejection.'
}
$payload = Make-Zip 'synthetic.zip' @(@('python.exe','synthetic non-executable interpreter fixture'),@('LICENSE.txt','fixture notice'))
$spec = [pscustomobject]@{sha256=(Get-PrivateSHA256 $payload); max_download_bytes=10000;max_unpacked_bytes=10000;max_entries=20;required=@('python.exe')}
$fetch = { param($item,$destination) [IO.File]::Copy($payload,$destination,$false) }
$data = Join-Path $case 'data'
$installed = Install-PrivatePython $data $spec $fetch
Require ([IO.File]::Exists($installed)) 'Synthetic bootstrap did not publish.'
Require ([IO.File]::ReadAllText((Join-Path ([IO.Path]::GetDirectoryName($installed)) 'LICENSE.txt')) -eq 'fixture notice') 'Notice was lost.'
$reused = Install-PrivatePython $data $spec {throw 'Must not fetch a verified interpreter again.'}
Require ($installed -eq $reused) 'Verified bootstrap reuse failed.'
$verifyOnly = Install-PrivatePython $data $spec {throw 'Verify-only cannot fetch.'} $false
Require ($verifyOnly -eq $installed) 'Verify-only refused valid certificate.'
$missing = Join-Path $case 'missing-tools'
Rejects { Install-PrivatePython $missing $spec {throw 'Verify-only cannot fetch.'} $false }
Require (@(Get-ChildItem -LiteralPath (Join-Path $missing 'managed-bootstrap') -ErrorAction SilentlyContinue).Count -eq 0) 'Verify-only made partial payload.'
[IO.File]::WriteAllText($installed,'damaged fixture')
Rejects { Install-PrivatePython $data $spec {throw 'Verify-only cannot repair.'} $false }
$repaired = Install-PrivatePython $data $spec $fetch
Require ([IO.File]::ReadAllText($repaired) -like 'synthetic*') 'Damaged output was trusted.'
Require (@(Get-ChildItem -Directory -LiteralPath (Join-Path $data 'managed-bootstrap') | Where-Object Name -Like '*.invalid-*').Count -eq 1) 'Invalid tool evidence was not preserved.'

$wrong = Join-Path $case 'wrong-data'
Rejects { Install-PrivatePython $wrong $spec {param($item,$destination) [IO.File]::WriteAllText($destination,'not the pinned bytes')} }
Require (-not (Test-Path -LiteralPath (Join-Path $wrong ('managed-bootstrap/'+$spec.sha256.Substring(0,24))))) 'Bad checksum promoted.'
$number=0
foreach ($names in @('../outside','C:/drive','dir./x','CON.txt','dir/a|Dir/b','x|x/y','A|a')) {
    $entries=@()
    foreach ($name in $names.Split('|')) { $entries += ,@($name,'x') }
    $zip = Make-Zip ('bad'+$number+'.zip') $entries
    $destination=Join-Path $case ('bad-out'+$number)
    Rejects { Expand-CheckedZip $zip $destination $spec }
    Require (-not (Test-Path -LiteralPath $destination)) 'Invalid ZIP created output before preflight.'
    ++$number
}
$zip=Make-Zip 'symlink.zip' -Entries (,@('link','../outside',([int]0xA1FF0000)))
Rejects { Expand-CheckedZip $zip (Join-Path $case 'symlink-out') $spec }
$limited = [pscustomobject]@{max_unpacked_bytes=1;max_entries=20;required=@('python.exe')}
Rejects { Expand-CheckedZip $payload (Join-Path $case 'over-budget') $limited }
$inputStream=New-Object IO.MemoryStream
$inputStream.Write((New-Object byte[] 100),0,100);$inputStream.Position=0
$outputStream=New-Object IO.MemoryStream
try {
    Rejects { Copy-CheckedStream $inputStream $outputStream 10 }
    Require ($outputStream.Length -le 10) 'Extractor wrote beyond declared size before refusing.'
} finally { $inputStream.Dispose();$outputStream.Dispose() }
# Junction creation is unprivileged on Windows and points only at another
# fixture directory. No external path is traversed or removed by these tests.
$outside=Join-Path $case 'other';[IO.Directory]::CreateDirectory($outside)|Out-Null
$junction=Join-Path $case 'junction'
New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
Rejects { Get-TreeCertificate $case }
Rejects { Assert-OwnedPath (Join-Path $junction 'setup.lock') $case }
Write-Output ('PASS private interpreter synthetic bootstrap: checksum, strict ZIP, notice, reuse, tamper, budgets; '+$number+' malicious ZIPs')
