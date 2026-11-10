# SPDX-License-Identifier: MIT
# Candidate must pass compile screening first. All GPU execution is Windows D3D12/PAL.
param([Parameter(Mandatory=$true)][string]$Candidate,
      [Parameter(Mandatory=$true)][string]$Name,
      [ValidateSet('Correctness','Short','LongCorrectness','Long','Profile')][string]$Stage='Correctness',
      [string]$Baseline='build/aco-runtime-trace/experiment-enc96-wave-rcp/data',
      [switch]$PerDataRecords,
      [int]$TraceFrame=-1)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $workspace
$tool=Join-Path $workspace 'build/aco-runtime-trace'
$original=Join-Path $workspace $Baseline
$variant=Join-Path $workspace "$Candidate/data"
$env:ONR_ACO_DIR=Join-Path $workspace "$Candidate/records/gfx1201/openrr-d3d12-v1"
if($PerDataRecords){
 foreach($dataRoot in @($original,$variant)){
  if(-not (Test-Path -LiteralPath (Join-Path $dataRoot 'aco/gfx1201/openrr-d3d12-v1/manifest.msgpack'))){throw 'Missing per-data records'}
 }
 Remove-Item Env:ONR_ACO_DIR -ErrorAction SilentlyContinue
}
$env:ONR_RR_FLOW='1'
foreach($envName in @('ONR_RR_STAMPS','ONR_RR_TRACE_KERNEL','ONR_RR_NO_SAMPLER','ONR_RR_DIRECT_GUIDES','ONR_RR_DIRECT')){
 Remove-Item -LiteralPath "Env:$envName" -ErrorAction SilentlyContinue
}
$warmup=16;$frames=48;$drs=24;$rounds=2;$all=0
if($Stage -eq 'Correctness'){$warmup=8;$frames=8;$drs=12;$rounds=1;$all=1}
if($Stage -eq 'LongCorrectness'){$warmup=8;$frames=280;$drs=12;$rounds=1;$all=1}
if($Stage -eq 'Long'){$warmup=48;$frames=240;$drs=64;$rounds=6}
if($Stage -eq 'Profile'){$warmup=24;$frames=640;$drs=24;$rounds=2;$env:ONR_RR_STAMPS='1'}
for($round=0;$round -lt $rounds;$round++){
 $reverse=($round%2)-eq 1
 $first=if($reverse){$variant}else{$original};$second=if($reverse){$original}else{$variant}
 $dest=Join-Path $tool "runs-next-optimization-2026-11-10/$Name/$Stage/round-$round"
 if(Test-Path -LiteralPath $dest){throw "Refusing to overwrite $dest"}
 New-Item -ItemType Directory -Path $dest -Force | Out-Null
 $argsBench=@('--runtime',(Join-Path $tool 'openrr-trace.asi'),'--baseline-runtime',(Join-Path $tool 'openrr-trace.asi'),
 '--baseline-data-dir',$first,'--data-dir',$second,'--baseline-compiler','aco-strict','--candidate-compiler','aco-strict',
 '--rw','1280','--rh','720','--ow','1920','--oh','1080','--warmup',"$warmup",'--frames',"$frames",'--drs-frames',"$drs",
 '--fast-mode','1','--optional-guides','0','--parity-all-frames',"$all",'--align-engine-phase','1','--trace-frame',"$TraceFrame",'--out',$dest)
 & (Join-Path $tool 'openrr_bench.exe') @argsBench 2>&1 | Out-File -LiteralPath (Join-Path $dest 'console.log') -Encoding utf8
 if($LASTEXITCODE -ne 0){throw "Candidate failed $Stage $round"}
 $summary=Get-Content -LiteralPath (Join-Path $dest 'summary.json') -Raw | ConvertFrom-Json
 if(-not $summary.parity_passed){throw 'Quality gate failed'}
 foreach($label in @('baseline','candidate')){
  $log=Get-Content -LiteralPath (Join-Path $dest "openrr_$label.log") -Raw
  $paths=[regex]::Matches($log,'runtime paths: direct_guides=\d+ direct_output=\d+ sampler=\d+ flow=\d+')
  if($paths.Count -eq 0){throw 'Missing path diagnostics'}
  foreach($path in $paths){if($path.Value -ne 'runtime paths: direct_guides=1 direct_output=1 sampler=1 flow=1'){throw 'Unexpected actual path'}}
 }
 @{round=$round;baseline_role=$(if($reverse){'variant'}else{'original'});candidate_role=$(if($reverse){'original'}else{'variant'});warmup=$warmup;steady=$frames;drs=$drs;candidate=$Candidate;baseline_data=$Baseline} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'order.json')
 Write-Output ("{0} {1} round-{2}: quality passed" -f $Name,$Stage,$round)
}
if($Stage -eq 'Profile'){Remove-Item Env:ONR_RR_STAMPS -ErrorAction SilentlyContinue}
