# Windows PowerShell5.1/.NET bootstrap. No Python, toolchain, admin or global changes.
param([string]$DataRoot, [string]$SourceRoot,
      [ValidateSet('tools','check','prepare','launch','status')][string]$Action='tools',
      [string]$Nonce='', [string]$Nsp='', [string]$Keys='', [string]$Update='',
      [switch]$Preview, [string]$PreviewPython='', [switch]$PreviewFail, [switch]$PreviewLifecycle, [switch]$PreviewRace,
      [switch]$Repair)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function Write-ToolsStage([string]$Message) {
    if ($Nonce) {
        Write-Output ('HWDER_UI:'+$Nonce+':'+(@{type='stage';message=$Message;stage='tools'} | ConvertTo-Json -Compress))
    } else { Write-Host $Message }
}

function Get-PrivateSHA256([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose();$stream.Dispose() }
}

function Assert-OwnedPath([string]$Path, [string]$Root) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if ($absolute -ne $base -and -not $absolute.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Private interpreter output escaped its data folder.'
    }
    $current = $absolute
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $entry = Get-Item -Force -LiteralPath $current
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Redirected data folders are unsupported.' }
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return $absolute
}

function Copy-CheckedStream($InputStream, $OutputStream, [long]$Expected) {
    $buffer=New-Object byte[] 1048576
    [long]$written=0
    while (($count=$InputStream.Read($buffer,0,$buffer.Length)) -gt 0) {
        if ($written+$count -gt $Expected) { throw 'Interpreter ZIP member exceeds declared size.' }
        $OutputStream.Write($buffer,0,$count)
        $written+=$count
    }
    if ($written -ne $Expected) { throw 'Truncated interpreter ZIP member.' }
}

function Expand-CheckedZip([string]$Archive, [string]$Destination, $Spec) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = @{}; $spelling = @{}; $entries = @(); [long]$total = 0
        if ($zip.Entries.Count -gt $Spec.max_entries) { throw 'Too many interpreter ZIP members.' }
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if ($name -match '[\\:<>"|?*\x00-\x1f]' -or $name.StartsWith('/')) { throw 'Unsafe interpreter ZIP path.' }
            $relative = $name.TrimEnd('/')
            $parts = $relative.Split('/')
            if (-not $relative) { throw 'Empty interpreter ZIP path.' }
            foreach ($part in $parts) {
                if (-not $part -or $part -in @('.', '..') -or $part -match '[ .]$' -or
                    $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') { throw 'Unsafe Windows ZIP path.' }
            }
            $kind = ($entry.ExternalAttributes -shr 16) -band 61440
            if (($entry.ExternalAttributes -band 1024) -or $kind -notin @(0,32768,16384)) { throw 'Link/special interpreter ZIP member.' }
            if ($seen.ContainsKey($relative)) { throw 'Duplicate interpreter ZIP member.' }
            $directory = $name.EndsWith('/')
            $seen[$relative] = $directory
            $prefix = ''
            foreach ($part in $parts) {
                if ($prefix) { $prefix += '/' }
                $prefix += $part
                if ($spelling.ContainsKey($prefix) -and $spelling[$prefix] -cne $prefix) { throw 'Case-colliding interpreter ZIP path.' }
                $spelling[$prefix] = $prefix
            }
            $total += $entry.Length
            if ($entry.Length -lt 0 -or $total -gt $Spec.max_unpacked_bytes) { throw 'Interpreter ZIP exceeds its unpacked limit.' }
            $entries += [pscustomobject]@{ Entry=$entry; Relative=$relative; Directory=$directory }
        }
        foreach ($item in $entries) {
            $parts = $item.Relative.Split('/')
            $prefix = ''
            for ($i=0; $i -lt $parts.Length-1; ++$i) {
                if ($prefix) { $prefix += '/' }
                $prefix += $parts[$i]
                if ($seen.ContainsKey($prefix) -and -not $seen[$prefix]) { throw 'ZIP file/directory collision.' }
            }
        }
        [IO.Directory]::CreateDirectory($Destination) | Out-Null
        Write-ToolsStage 'Extracting private Python' | Out-Host
        foreach ($item in $entries) {
            $target = Assert-OwnedPath (Join-Path $Destination $item.Relative) $Destination
            if ($item.Directory) { [IO.Directory]::CreateDirectory($target) | Out-Null; continue }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            $inputStream = $item.Entry.Open()
            $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew)
            try { Copy-CheckedStream $inputStream $outputStream $item.Entry.Length } finally { $outputStream.Dispose(); $inputStream.Dispose() }
            if ((Get-Item -LiteralPath $target).Length -ne $item.Entry.Length) { throw 'Truncated interpreter ZIP member.' }
        }
    } finally { $zip.Dispose() }
    foreach ($required in $Spec.required) {
        if (-not [IO.File]::Exists((Join-Path $Destination $required))) { throw 'Interpreter archive is incomplete.' }
    }
}

function Get-TreeCertificate([string]$Root) {
    $result = @{}
    Assert-OwnedPath $Root $Root | Out-Null
    $queue=New-Object 'Collections.Generic.Queue[string]'
    $queue.Enqueue($Root)
    $clock=[Diagnostics.Stopwatch]::StartNew();[long]$last=-500
    while ($queue.Count) {
        $directory=$queue.Dequeue()
        foreach ($entry in Get-ChildItem -Force -LiteralPath $directory) {
            Assert-OwnedPath $entry.FullName $Root | Out-Null
            if ($entry.PSIsContainer) { $queue.Enqueue($entry.FullName);continue }
            $relative = $entry.FullName.Substring($Root.TrimEnd('\').Length+1).Replace('\','/')
            $result[$relative] = Get-PrivateSHA256 $entry.FullName
            if ($result.Count -eq 1 -or $clock.ElapsedMilliseconds-$last -ge 500) {
                Write-ToolsStage ('Checking private Python: '+$result.Count+' files hashed') | Out-Host
                $last=$clock.ElapsedMilliseconds
            }
        }
    }
    Write-ToolsStage ('Checked private Python file hashes: '+$result.Count+' (scan complete)') | Out-Host
    return $result
}

function Install-PrivatePython([string]$Data, $Spec, [scriptblock]$Fetch, [bool]$AllowDownload=$true) {
    $data = Assert-OwnedPath $Data $Data
    [IO.Directory]::CreateDirectory($data) | Out-Null
    $lockPath = Assert-OwnedPath (Join-Path $data 'setup.lock') $data
    # Same byte-range lock as first_run.setup_lock. This bootstrap never holds it
    # while invoking the Python coordinator (which acquires it independently).
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    $locked = $false
    try {
        while (-not $locked) {
            try { $lock.Lock(0,1); $locked=$true } catch [IO.IOException] { Start-Sleep -Milliseconds 250 }
        }
        $root = Assert-OwnedPath (Join-Path $data 'managed-bootstrap') $data
        [IO.Directory]::CreateDirectory($root) | Out-Null
        $leaf = $Spec.sha256.Substring(0,24)
        $final = Assert-OwnedPath (Join-Path $root $leaf) $data
        $certificate = Join-Path $final 'complete.json'
        if ([IO.File]::Exists($certificate)) {
            Write-ToolsStage 'Checking the installed private Python interpreter' | Out-Host
            $old = Get-Content -Raw -LiteralPath $certificate | ConvertFrom-Json
            $files = Get-TreeCertificate (Join-Path $final 'output')
            $oldNames = @($old.files.PSObject.Properties.Name)
            $equal = $old.owner -eq 'hwder-private-python-v1' -and $old.sha256 -eq $Spec.sha256 -and $oldNames.Count -eq $files.Count
            foreach ($name in $oldNames) { if ($old.files.$name -ne $files[$name]) { $equal=$false } }
            if ($equal -and [IO.File]::Exists((Join-Path $final 'output/python.exe'))) {
                Write-ToolsStage 'Private Python interpreter verified' | Out-Host
                return (Join-Path $final 'output/python.exe')
            }
        }
        if (-not $AllowDownload) { throw 'Private tools are not ready. Choose Install private tools, then recheck.' }
        $attempt = Join-Path $root ($leaf + '.partial-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
        [IO.Directory]::CreateDirectory($attempt) | Out-Null
        $archive = Join-Path $attempt 'python.zip'
        & $Fetch $Spec $archive
        Write-ToolsStage 'Checking the private Python download checksum' | Out-Host
        if ((Get-Item -LiteralPath $archive).Length -gt $Spec.max_download_bytes -or
            (Get-PrivateSHA256 $archive) -ne $Spec.sha256) {
            throw 'Private interpreter checksum mismatch.'
        }
        $output = Join-Path $attempt 'output'
        Expand-CheckedZip $archive $output $Spec
        $record = @{owner='hwder-private-python-v1';sha256=$Spec.sha256;files=(Get-TreeCertificate $output)}
        Write-ToolsStage 'Finalizing the verified private Python interpreter' | Out-Host
        [IO.File]::WriteAllText((Join-Path $attempt 'complete.json'), ($record | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
        if ([IO.Directory]::Exists($final)) { [IO.Directory]::Move($final, $final+'.invalid-'+[Guid]::NewGuid().ToString('N').Substring(0,8)) }
        [IO.Directory]::Move($attempt,$final)
        Write-ToolsStage 'Private Python interpreter installed and verified' | Out-Host
        return (Join-Path $final 'output/python.exe')
    } finally {
        if ($locked) { $lock.Unlock(0,1) }
        $lock.Dispose()
    }
}

function Invoke-PrivateBootstrap {
    if (-not $DataRoot -or -not $SourceRoot) { throw 'Choose a data folder and complete source kit.' }
    if ($Nonce -and $Nonce -notmatch '^[0-9a-f]{32}$') { throw 'Invalid setup session identifier.' }
    $source = [IO.Path]::GetFullPath($SourceRoot)
    if ($Preview) {
        # Explicit native UI preview only: forces synthetic bridge events;
        # cannot invoke installers, preparation, or gameplay.
        if (-not [IO.File]::Exists($PreviewPython)) { throw 'No preview interpreter.' }
        $arguments=@('-B','-X','utf8','-u',(Join-Path $source 'tools/setup_bridge.py'),
                     '--demo','--action',$(if($Action -eq 'tools'){'components'}else{$Action}),
                     '--data',$DataRoot,'--ui-protocol','--nonce',$Nonce)
        if ($PreviewFail) { $arguments+='--demo-fail' }
        if ($PreviewLifecycle) { $arguments+='--demo-lifecycle' }
        if ($PreviewRace) { $arguments+='--demo-lifecycle-race' }
        & $PreviewPython @arguments
        return
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $source 'tools/managed_toolchain.json') | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.owner -ne 'hwder-private-tools-v1') { throw 'Unknown tool manifest.' }
    $spec = @($manifest.artifacts | Where-Object id -eq 'python')
    if ($spec.Count -ne 1 -or $spec[0].url -ne 'https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip' -or
        $spec[0].sha256 -ne '97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297' -or
        $spec[0].max_download_bytes -gt 20000000 -or $spec[0].max_unpacked_bytes -gt 100000000 -or $spec[0].max_entries -gt 50000) {
        throw 'Unreviewed interpreter pin or bounds.'
    }
    $fetch = {
        param($item,$destination)
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $request = [Net.HttpWebRequest]::Create($item.url)
        $request.AllowAutoRedirect = $false
        $request.Timeout=30000
        $response=$request.GetResponse()
        try {
            if ([int]$response.StatusCode -ne 200) { throw 'Interpreter download refused.' }
            $inputStream=$response.GetResponseStream()
            $outputStream=[IO.File]::Open($destination,[IO.FileMode]::CreateNew)
            try {
                $buffer=New-Object byte[] 1048576; [long]$total=0
                while (($count=$inputStream.Read($buffer,0,$buffer.Length)) -gt 0) {
                    $total+=$count
                    if ($total -gt $item.max_download_bytes) { throw 'Interpreter download exceeded its limit.' }
                    $outputStream.Write($buffer,0,$count)
                    Write-ToolsStage ('Downloading private Python: '+[math]::Round($total/1048576,1)+' MiB received') | Out-Host
                }
            } finally { $outputStream.Dispose(); $inputStream.Dispose() }
        } finally { $response.Dispose() }
    }
    Write-ToolsStage $(if($Action -eq 'tools'){'Verifying or installing private tools'}else{'Verifying your private setup interpreter'})
    $python = Install-PrivatePython $DataRoot $spec[0] $fetch ($Action -eq 'tools')
    if ($Action -in @('launch','status')) {
        # Play only needs the completely certified small interpreter, source
        # helpers and ready executable; compiler/wheel files are not loaded.
        Write-ToolsStage 'Verifying your prepared game'
        $entry=(Join-Path $source 'tools/setup_bridge.py').Replace('\','\\').Replace("'","\'")
        $arguments=@('-I','-B','-X','utf8','-u','-c',("import runpy;runpy.run_path('"+$entry+"',run_name='__main__')"),
                     '--action',$Action,'--data',$DataRoot,'--managed-launch','--ui-protocol')
        if ($Nonce) { $arguments+=@('--nonce',$Nonce) }
        & $python @arguments
        return
    }
    # The embeddable _pth intentionally ignores arbitrary PYTHONPATH. run_path
    # enters the checked-in module, which explicitly adds its own tools folder.
    $entry = (Join-Path $source 'tools/managed_tools.py').Replace('\','\\').Replace("'","\'")
    $arguments=@('-I','-B','-X','utf8','-u','-c',("import runpy;runpy.run_path('"+$entry+"',run_name='__main__')"),
                 '--data',$DataRoot,'--source',$source)
    if ($Nonce) { $arguments+=@('--nonce',$Nonce) }
    if ($Action -eq 'tools') { $arguments+='--install'; if ($Repair) { $arguments+='--repair' } } else { $arguments+=@('--bridge-action',$Action) }
    if ($Nsp) { $arguments+=@('--nsp',$Nsp) }
    if ($Keys) { $arguments+=@('--keys',$Keys) }
    if ($Update) { $arguments+=@('--update',$Update) }
    & $python @arguments
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        Invoke-PrivateBootstrap
        exit $LASTEXITCODE
    } catch {
        Write-Host $_.Exception.Message
        if ($Nonce -match '^[0-9a-f]{32}$') {
            Write-Output ('HWDER_UI:'+$Nonce+':'+(@{type='failed';message='Private tools are not installed yet. Choose Install (about 260 MB, no administrator access).';next='install'} | ConvertTo-Json -Compress))
        }
        exit 1
    }
}
