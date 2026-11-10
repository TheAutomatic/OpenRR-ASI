# SPDX-License-Identifier: MIT
# ACO versus ACO, no intermediate tracing or per-frame readbacks.
param([string]$Output='build/aco-runtime-trace/runs-approved-2026-11-10/prefetch-timing')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $root
$tool=Join-Path $root 'build/aco-runtime-trace'
$runtime=Join-Path $tool 'openrr-trace.asi'
$original=Join-Path $root 'build/asi/stage/openrr-amd'
$variant=Join-Path $tool 'experiment-projection-prefetch/data'
$env:ONR_ACO_DIR=Join-Path $tool 'experiment-projection-prefetch/records/gfx1201/openrr-d3d12-v1'
$env:ONR_RR_FLOW='1'
foreach($name in @('ONR_RR_TRACE_KERNEL','ONR_RR_NO_SAMPLER','ONR_RR_DIRECT_GUIDES','ONR_RR_DIRECT')) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}
foreach($round in 0..5) {
    $reverse=($round % 2) -eq 1
    $first=if($reverse){$variant}else{$original}
    $second=if($reverse){$original}else{$variant}
    $dest=Join-Path $Output "round-$round"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    $benchArgs=@('--runtime',$runtime,'--baseline-runtime',$runtime,
        '--baseline-data-dir',$first,'--data-dir',$second,
        '--baseline-compiler','aco-strict','--candidate-compiler','aco-strict',
        '--rw','1280','--rh','720','--ow','1920','--oh','1080',
        '--warmup','24','--frames','48','--drs-frames','24',
        '--fast-mode','1','--optional-guides','0','--parity-all-frames','0',
        '--align-engine-phase','1','--trace-frame','-1','--out',$dest)
    & (Join-Path $tool 'openrr_bench.exe') @benchArgs 2>&1 | Tee-Object -FilePath (Join-Path $dest 'console.log') | Out-Host
    if($LASTEXITCODE -ne 0){throw "Timing failed in round $round"}
    foreach($label in @('baseline','candidate')) {
        $log=Get-Content -LiteralPath (Join-Path $dest "openrr_$label.log") -Raw
        $paths=[regex]::Matches($log,'runtime paths: direct_guides=\d+ direct_output=\d+ sampler=\d+ flow=\d+')
        if($paths.Count -eq 0){throw 'Missing path diagnostics'}
        foreach($path in $paths) {
            if($path.Value -ne 'runtime paths: direct_guides=1 direct_output=1 sampler=1 flow=1'){throw 'Unexpected actual path'}
        }
    }
    @{round=$round;baseline_role=$(if($reverse){'variant'}else{'original'});candidate_role=$(if($reverse){'original'}else{'variant'});warmup=24;steady=48;drs=24} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'order.json')
}
