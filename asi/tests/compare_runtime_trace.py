#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare actual-runtime readbacks. Raw bytes stay authoritative; half-zero
classification is reported only for surfaces known to contain FP16 values.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct


def compare(a, b, half=False):
    if len(a) != len(b):
        raise ValueError(f"size mismatch {len(a)} != {len(b)}")
    first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
    result = dict(bytes=len(a), differing_bytes=sum(x != y for x, y in zip(a, b)), first_byte=first)
    if half:
        zeros = nonzero = nonfinite = 0
        first_nonzero = None
        for i, (x,), (y,) in zip(range(len(a)//2), struct.iter_unpack('<H', a), struct.iter_unpack('<H', b)):
            nonfinite += ((x & 0x7c00) == 0x7c00) + ((y & 0x7c00) == 0x7c00)
            if x == y:
                continue
            if (x & 0x7fff) == 0 and (y & 0x7fff) == 0:
                zeros += 1
            else:
                nonzero += 1
                if first_nonzero is None:
                    first_nonzero = dict(element=i, baseline_bits=f'{x:04x}', candidate_bits=f'{y:04x}',
                                         baseline=struct.unpack('<e', struct.pack('<H', x))[0],
                                         candidate=struct.unpack('<e', struct.pack('<H', y))[0])
        result.update(signed_zero_elements=zeros, nonzero_elements=nonzero, nonfinite_values=nonfinite,
                      first_nonzero=first_nonzero)
    return result


def fields(text):
    return dict(re.findall(r'(\S+)=([^\s]+)', text))


def is_half_surface(name):
    # Down/pooled-history are packed R10G10B10A2, despite the history suffix.
    suffixes = ('exposure', 'history', 'dfn', 'dfn-pool', 'rg', 'b', 'tile')
    return (name == 'output.bin' or any(name.endswith('-'+s+'.bin') for s in suffixes)) and not name.endswith('-pooled-history.bin')


def pointer_offsets(kernel):
    """Byte offsets from kernels.hpp / runtime.cpp scalar-layout PC structs.
    These slots need resource+offset review, never a heuristic integer mask.
    Unknown interfaces fail closed rather than ignoring their push parameters.
    """
    kernel = kernel.removesuffix('_lt')
    block = list(range(0, 88, 8)) + [112]
    if kernel.startswith(('enc0_attr', 'd3d_enc0_attr')):
        return list(range(0, 160, 8)) + [208, 216, 224]
    if kernel.startswith('enc0_stage'):
        return block + list(range(120, 184, 8)) + [200, 208]
    if kernel.startswith('enc_stage'):
        return block + list(range(120, 184, 8))
    if kernel.startswith('dec_up'):
        return block + [120, 128, 136]
    if kernel.startswith(('dec_dfn', 'dec_head')):
        return block + list(range(120, 160, 8)) + [176, 184, 192, 200, 224]
    if kernel in ('ae','d3d_ae'):
        return [0, 8, 40, 48, 56]
    if kernel in ('ingest','d3d_ingest'):
        return [16] + list(range(24, 88, 8))
    if kernel == 'clear_range':
        return [0]
    if kernel == 'pooled_history2':
        return [0, 8]
    if kernel in ('post_dlt','post_dlt_f'):
        return list(range(0, 104, 8)) + [184, 192]
    if kernel in ('post','post_f'):
        return list(range(0, 104, 8))
    if kernel == 'post_tile':
        return [0, 8, 16, 24]
    if kernel in ('downsample','d3d_downsample'):
        # d3d variant's dst slot holds packed output origin, not an address.
        return [0, 8, 24] if kernel.startswith('d3d_') else [0, 8, 16, 24]
    if kernel in ('pack','d3d_pack'):
        return [0] if kernel.startswith('d3d_') else [0, 8]
    raise ValueError(f'push interface needs explicit review: {kernel}')


def validate_metadata(texts):
    frames=[]
    for text in texts:
        matches=re.findall(r'^frame (.+)$',text,re.M)
        if len(matches)!=1:raise ValueError('expected one frame record')
        frames.append(fields(matches[0]))
    for field in ('engine','phase8','fresh','reset','lite','render','jitter','motion-scale','depth-sign'):
        if any(field not in f for f in frames) or frames[0][field]!=frames[1][field]:
            raise ValueError(f'incomparable frame state: {field}')
    for name in ('view-to-clip','world-to-view'):
        matrices=[]
        for text in texts:
            found=re.findall(rf'^{name}=(.+)$',text,re.M)
            if len(found)!=1:raise ValueError(f'missing/duplicate matrix: {name}')
            matrix=[float(v) for v in found[0].split(',') if v]
            if len(matrix)!=16 or not all(math.isfinite(v) for v in matrix):raise ValueError(f'invalid matrix: {name}')
            matrices.append(matrix)
        if matrices[0]!=matrices[1]:raise ValueError(f'different matrix: {name}')
    for kind,ignored in (('guide',{'buffer','resource'}),('texture',set())):
        records=[]
        for text in texts:
            rows={}
            for name,body in re.findall(rf'^{kind} (\S+) (.+)$',text,re.M):
                if name in rows:raise ValueError(f'duplicate {kind}: {name}')
                parsed=fields(body)
                required=({'present','direct','format','pitch','origin','texture-size','view-format','buffer','resource'} if kind=='guide'
                          else {'format','width','height','row-bytes','origin'})
                if not required.issubset(parsed):raise ValueError(f'incomplete {kind} metadata: {name}')
                rows[name]={k:v for k,v in parsed.items() if k not in ignored}
            if not rows:raise ValueError(f'missing {kind} metadata')
            records.append(rows)
        if records[0]!=records[1]:raise ValueError(f'different {kind} metadata')
    dispatches=[]
    for text in texts:
        rows=[]
        for tag,body in re.findall(r'^dispatch (\S+) (.*)$',text,re.M):
            f=fields(body)
            if set(f)!={'aco','indirect','groups','push'}:raise ValueError(f'incomplete dispatch: {tag}')
            groups=tuple(int(v) for v in f['groups'].split(','))
            if len(groups)!=3 or f['indirect'] not in ('0','1'):raise ValueError(f'invalid dispatch: {tag}')
            rows.append(dict(tag=tag,indirect=int(f['indirect']),groups=groups,push=bytes.fromhex(f['push'])))
        if not rows:raise ValueError('missing dispatches')
        dispatches.append(rows)
    if len(dispatches[0])!=len(dispatches[1]):raise ValueError('different dispatch count')
    pending=[]
    for a,b in zip(*dispatches):
        for field in ('tag','indirect','groups'):
            if a[field]!=b[field]:raise ValueError(f'different dispatch {field}: {a["tag"]}')
        if len(a['push'])!=len(b['push']):raise ValueError(f'different push size: {a["tag"]}')
        offsets=pointer_offsets(a['tag'].split('-',1)[1])
        address_bytes=set()
        for off in offsets:
            if off+8>len(a['push']):raise ValueError(f'push schema out of bounds: {a["tag"]} byte {off}')
            address_bytes.update(range(off,off+8))
            values=[int.from_bytes(d['push'][off:off+8],'little') for d in (a,b)]
            if values[0]!=values[1]:
                pending.append(dict(dispatch=a['tag'],push_byte=off,baseline=values[0],candidate=values[1]))
        if any(x!=y for i,(x,y) in enumerate(zip(a['push'],b['push'])) if i not in address_bytes):
            raise ValueError(f'different non-address push parameters: {a["tag"]}')
    return dict(frame=frames,dispatch_count=len(dispatches[0]),non_address_parameters_checked=True,
                input_equivalence_established=False,address_review_required=True,pending_address_pairs=pending,
                address_policy='No automatic address equivalence claimed. For every pointer slot, resolve each value against its owning resource/view and byte offset using trace view/root metadata; check unlisted weights/aliases against kernels.hpp and runtime allocation metadata. Nulls and descriptor-backed references also need review. Raw push bytes remain in trace.txt.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline', type=Path)
    p.add_argument('candidate', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    texts = [(root/'trace.txt').read_text() for root in (args.baseline,args.candidate)]
    validation=validate_metadata(texts)
    print('Non-address metadata checked; resource/offset address review is still required. Input equivalence is not yet established.')
    paths = [dict((p.name,p) for p in root.glob('*.bin')) for root in (args.baseline,args.candidate)]
    if paths[0].keys() != paths[1].keys():
        raise ValueError('readback coverage differs')
    def key(n):
        return (int(n.split('-')[0])+1 if n[0].isdigit() else 0 if n.startswith('input-') else 10000,n)
    rows=[]
    for name in sorted(paths[0],key=key):
        # Parameters and guide tables contain allocation addresses. Their raw
        # differences are logged but cannot establish a numerical divergence.
        address_data = name.endswith(('-parameters.bin','-guide-table.bin'))
        half = is_half_surface(name)
        a,b=(paths[i][name].read_bytes() for i in (0,1))
        row=dict(name=name,address_data=address_data,**compare(a,b,half))
        rows.append(row)
        if row['differing_bytes']:
            print(json.dumps(row,allow_nan=True))
    report=dict(metadata_validation=validation,notes=[
        'Readbacks add transitions and serialize flow dispatches. Verify final output against the untraced run.',
        'Allocation-sized snapshots may contain unwritten padding; inspect valid pixel/channel ranges before naming a first numerical divergence.',
        'Mixed packed records and FP8 tensors are byte comparisons, not FP16 values.',
        'Cross-process traces compare independent histories; differing frame inputs must be resolved before blaming a dispatch.'
    ],buffers=rows)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(report,indent=2,allow_nan=True)+'\n')
    print(f"compared {len(rows)} readbacks; differing={sum(r['differing_bytes']!=0 for r in rows)}")


if __name__ == '__main__':
    main()
