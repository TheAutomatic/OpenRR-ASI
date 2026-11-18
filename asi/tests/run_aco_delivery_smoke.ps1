# SPDX-License-Identifier: MIT
# Package integration only; printed timings are not a performance baseline.
param([string]$Build='build/asi-delivery-candidate', [string]$Output='build/aco-delivery-review/package-smoke',
      [string]$ProbeData='build/aco-delivery-review/probe-data', [switch]$Resume)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $workspace
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$data=Join-Path $buildRoot 'stage/openrr-amd'
$plugin=Join-Path $buildRoot 'stage/plugins/openrr-amd.asi'
if((Test-Path -LiteralPath $Output) -and -not $Resume){throw 'Refusing to overwrite smoke evidence'}
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$saved=@{}
$names=@('ONR_ACO_DIR','ONR_RR_FLOW','ONR_RR_DIRECT','ONR_RR_DIRECT_GUIDES','ONR_RR_NO_SAMPLER','ONR_RR_TRACE_KERNEL','ONR_RR_TRACE_PATH','ONR_RR_STAMPS','ONR_RR_BACKEND','OPENRR_ACO_PROBE','OPENRR_ACO_COMPARE','OPENRR_ACO_API_PROBE','OPENRR_SPV_DIR')
foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process');Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue}
try {
 $env:ONR_RR_FLOW='1'
 $env:OPENRR_ACO_PROBE='1'
 $probeRoot=(Resolve-Path -LiteralPath $ProbeData).Path
 $env:OPENRR_SPV_DIR=Join-Path $probeRoot 'shaders'
 $env:ONR_ACO_DIR=Join-Path $probeRoot 'aco/gfx1201/openrr-d3d12-v1'
 & (Join-Path $buildRoot 'test_asi_units.exe') 2>&1 | Out-File -LiteralPath (Join-Path $Output 'abi-probe.log') -Encoding utf8
 if($LASTEXITCODE -ne 0){throw 'Candidate-linked ACO ABI probe failed'}
 Remove-Item Env:OPENRR_ACO_PROBE
 Remove-Item Env:OPENRR_SPV_DIR
 Remove-Item Env:ONR_ACO_DIR
 foreach($mode in @(1,0)){
  $dest=Join-Path $Output "mode-$mode"
  $argsBench=@('--runtime',$plugin,'--data-dir',$data,'--candidate-compiler','env','--rw','1280','--rh','720','--ow','1920','--oh','1080','--warmup','8','--frames','8','--drs-frames','12','--fast-mode',"$mode",'--optional-guides','0','--parity-all-frames','1','--align-engine-phase','0','--trace-frame','-1','--out',$dest)
  if(-not ($Resume -and (Test-Path -LiteralPath (Join-Path $dest 'summary.json')))){
   & (Join-Path $buildRoot 'openrr_bench.exe') @argsBench 2>&1 | Out-File -LiteralPath (Join-Path $Output "mode-$mode-console.log") -Encoding utf8
   if($LASTEXITCODE -ne 0){throw "Production module smoke failed: mode-$mode"}
  }
  $log=Get-Content -LiteralPath (Join-Path $dest 'openrr_candidate.log') -Raw
  if($log -notmatch 'compiler=ACO' -or $log -match 'compiler=LLPC'){throw 'Default D3D12 selection did not remain all ACO'}
  if($log -notmatch 'runtime paths: direct_guides=1 direct_output=1 sampler=1 flow=1'){throw 'Unexpected actual runtime paths'}
  if($log -match 'runtime paths: (?!direct_guides=1 direct_output=1 sampler=1 flow=1)'){throw 'Runtime paths changed'}
  if($mode -eq 1){
   $expected=@{enc_stage_96_128_128_fl_lt='c08996618a61cd08';dec_dfn_96_128_fl_lt='6a4ccda0cd2efb14';enc_stage_128_128_160_fl_lt='591c303d1916df59'}
   foreach($shader in $expected.Keys){if($log -notmatch ([regex]::Escape($shader)+' spv=[0-9a-f]+ compiler=ACO [^\r\n]* code='+$expected[$shader]+' ')){throw "Wrong retained code: $shader"}}
  }
  $hashes=@()
  foreach($phase in @('steady','drs')){
   $count=if($phase -eq 'steady'){8}else{12}
   for($i=0;$i -lt $count;$i++){
    $path=Join-Path $dest "candidate-$phase-$i.bin"
    $hashes+=@{file=[IO.Path]::GetFileName($path);sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;bytes=(Get-Item -LiteralPath $path).Length}
   }
  }
  $hashFile=Join-Path $dest 'output-hashes.json'
  $hashes | ConvertTo-Json | Set-Content -LiteralPath $hashFile -Encoding utf8
  $verified=Get-Content -LiteralPath $hashFile -Raw | ConvertFrom-Json
  $destRoot=(Resolve-Path -LiteralPath $dest).Path
  foreach($item in $verified){
   $path=(Resolve-Path -LiteralPath (Join-Path $destRoot $item.file)).Path
   if(-not $path.StartsWith($destRoot+[IO.Path]::DirectorySeparatorChar)){throw 'Readback outside smoke directory'}
   if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.sha256){throw 'Readback hash mismatch'}
  }
  foreach($item in $verified){Remove-Item -LiteralPath (Join-Path $destRoot $item.file)}
  Write-Output ("Production module mode-{0}: 20 finite frame outputs; no new parity or performance conclusion" -f $mode)
 }
 # The ASI shell may append one exit log beside the module. Archive that exact
 # generated file, then remove it so the candidate remains a clean package.
 $shellLog=Join-Path $buildRoot 'stage/plugins/openrr-amd.log'
 if(Test-Path -LiteralPath $shellLog){
  $copy=Join-Path $Output 'asi-shell-exit.log'
  Copy-Item -LiteralPath $shellLog -Destination $copy
  if((Get-FileHash -LiteralPath $shellLog).Hash -ne (Get-FileHash -LiteralPath $copy).Hash){throw 'Shell log archive mismatch'}
  Remove-Item -LiteralPath $shellLog
 }
} finally {
 foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
