param([string]$Output = 'build/aco-runtime-trace/runs',
      [ValidateSet('Baseline','Control','Trace')][string]$Stage = 'Baseline', [int]$TraceFrame = 8,
      [string]$DataDir = 'build/asi/stage/openrr-amd',
      [string]$Records = 'build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1',
      [string]$TraceName = '', [string]$ReferenceName = 'fast-aligned')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location -LiteralPath $root
$tool = Join-Path $root 'build/aco-runtime-trace'
$runtime = Join-Path $tool 'openrr-trace.asi'
$data = Join-Path $root $DataDir
$env:ONR_ACO_DIR = Join-Path $root $Records
$env:ONR_RR_FLOW = '1'
Remove-Item Env:ONR_RR_TRACE_KERNEL -ErrorAction SilentlyContinue
Remove-Item Env:ONR_RR_NO_SAMPLER -ErrorAction SilentlyContinue
Remove-Item Env:ONR_RR_DIRECT_GUIDES -ErrorAction SilentlyContinue
Remove-Item Env:ONR_RR_DIRECT -ErrorAction SilentlyContinue
# Correctness only. Printed timestamps are not speed comparisons.
# Trace numbering begins at warmup 0 (8..15 steady,16..27 DRS).
function Run-Case([string]$Name,[int]$Fast,[int]$Align,[int]$Trace) {
    $dest = Join-Path $Output $Name
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    $benchArgs = @('--runtime',$runtime,'--baseline-runtime',$runtime,
        '--data-dir',$data,'--baseline-data-dir',$data,'--baseline-compiler','llpc','--candidate-compiler','aco-strict',
        '--rw','1280','--rh','720','--ow','1920','--oh','1080','--warmup','8','--frames','8','--drs-frames','12',
        '--fast-mode',"$Fast",'--optional-guides','0','--parity-all-frames','1',
        '--align-engine-phase',"$Align",'--trace-frame',"$Trace",'--out',$dest)
    & (Join-Path $tool 'openrr_bench.exe') @benchArgs 2>&1 | Tee-Object -FilePath (Join-Path $dest 'console.log') | Out-Host
    if($LASTEXITCODE -ne 0){throw "Correctness case failed: $Name exit=$LASTEXITCODE"}
    foreach($label in @('baseline','candidate')) {
        $log = Get-Content -LiteralPath (Join-Path $dest "openrr_$label.log") -Raw
        $required = @(
            "D3D12 backend: the guides are read from the game's textures and the output written in place (check passed)",
            'D3D12 backend: the post samples its history through the sampler, as on Vulkan (check passed)',
            'runtime paths: direct_guides=1 direct_output=1 sampler=1 flow=1')
        foreach($line in $required) {
            if(-not $log.Contains($line)){throw "Actual path verification failed: $Name / $label missing $line"}
        }
        $paths = [regex]::Matches($log,'runtime paths: direct_guides=\d+ direct_output=\d+ sampler=\d+ flow=\d+')
        foreach($path in $paths) {
            if($path.Value -ne $required[2]){throw "Actual path changed: $Name / $label $($path.Value)"}
        }
    }
    $summary = Get-Content -LiteralPath (Join-Path $dest 'summary.json') -Raw | ConvertFrom-Json
    if(-not $summary.parity_passed){throw "Parity failed: $Name"}
    if($Fast -eq 0 -and @($summary.parity.PSObject.Properties | Where-Object { $_.Value.max_ulp_diff -ne 0 -or $_.Value.exact_percent -ne 100 }).Count) {
        throw "Legacy is not exact: $Name"
    }
    return $summary
}
# Run only after the user confirms the machine is idle. Inspect all three
# summaries and frame-state CSVs before choosing a trace frame.
if($Stage -eq 'Baseline') {
    $original = Run-Case 'fast-original-sequence' 1 0 -1
    if(-not @($original.parity.PSObject.Properties | Where-Object { $_.Value.max_abs_err -gt 0 }).Count) {
        throw 'Original runtime nonzero difference was not reproduced; inspect before tracing'
    }
    $null = Run-Case 'fast-aligned' 1 1 -1
    $null = Run-Case 'legacy-aligned' 0 1 -1
    Write-Host 'Baseline phase complete. Inspect Fast reproduction, aligned state and Legacy exactness before Stage Trace.'
} elseif($Stage -eq 'Control') {
    if(-not $TraceName){throw 'Control requires an explicit TraceName'}
    $null = Run-Case $TraceName 1 1 -1
} else {
    foreach($name in @('fast-original-sequence','fast-aligned','legacy-aligned')) {
        $prior = Get-Content -LiteralPath (Join-Path $Output "$name/summary.json") -Raw | ConvertFrom-Json
        if(-not $prior.parity_passed){throw "Baseline gate failed: $name"}
    }
    $name = if($TraceName){$TraceName}else{"fast-trace-$TraceFrame"}
    $null = Run-Case $name 1 1 $TraceFrame
    # A trace can perturb the flow chain. Compare each compiler against its own
    # untraced aligned run before interpreting any intermediate divergence.
    foreach($label in @('baseline','candidate')) {
        foreach($phase in @('steady','drs')) {
            $count = if($phase -eq 'steady'){8}else{12}
            for($i=0;$i -lt $count;$i++) {
                $file = "$label-$phase-$i.bin"
                $a = (Get-FileHash -LiteralPath (Join-Path $Output "$ReferenceName/$file") -Algorithm SHA256).Hash
                $b = (Get-FileHash -LiteralPath (Join-Path $Output "$name/$file") -Algorithm SHA256).Hash
                if($a -ne $b){throw "Traced/untraced output differs: $file; do not attribute first divergence from this trace"}
            }
        }
    }
    Write-Host 'Traced/untraced measured outputs match per compiler. Check entry histories before attributing a dispatch.'
}
