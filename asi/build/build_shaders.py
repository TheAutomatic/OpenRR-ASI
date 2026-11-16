#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Compiles RUNTIME_SPVS and LITE_SPVS (reading gpu/shaders.list + gpu/CMakeLists.txt + gpu/fastmode.cmake) into the target directory.
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

def parse_fastmode(cmake_path):
    lt_defs = {}
    text = open(cmake_path, "r", encoding="utf-8").read()
    for m in re.finditer(r"set\(\s*((?:LT|LX)_DEFS_[A-Za-z0-9_]+)\s+([^)]+)\)", text):
        key = m.group(1)
        defs = m.group(2).split()
        lt_defs[key] = defs
    return lt_defs

def parse_shader_lists(gpu_dir):
    list_path = os.path.join(gpu_dir, "shaders.list")
    cmake_path = os.path.join(gpu_dir, "CMakeLists.txt")
    if not os.path.isfile(list_path):
        raise RuntimeError(f"missing shader list file: {list_path}")
    plain_from_list = []
    for line in open(list_path, "r", encoding="utf-8"):
        s = line.strip()
        if s and not s.startswith("#"):
            plain_from_list.append(s)
    if not plain_from_list:
        raise RuntimeError(f"empty shader list in {list_path}")

    cmake_text = open(cmake_path, "r", encoding="utf-8").read()
    m_flow = re.search(r"set\(\s*FLOW_KERNELS\s+([^)]+)\)", cmake_text, re.DOTALL)
    if not m_flow:
        raise RuntimeError(f"FLOW_KERNELS not found in {cmake_path}")
    flow_kernels = m_flow.group(1).split()

    # If CMakeLists.txt also lists explicit foreach(n ...) shaders instead of PLAIN_RUNTIME_SHADERS, enforce consistency.
    m_foreach = re.search(r"foreach\(\s*n\s+([^)]+)\)\s*list\(\s*APPEND\s+RUNTIME_SPVS", cmake_text, re.DOTALL)
    if m_foreach:
        tokens = m_foreach.group(1).split()
        if tokens != ["${PLAIN_RUNTIME_SHADERS}"]:
            if set(tokens) != set(plain_from_list):
                raise RuntimeError(f"shader list mismatch between {list_path} ({plain_from_list}) and {cmake_path} ({tokens})")

    for name in plain_from_list:
        comp_path = os.path.join(gpu_dir, "shaders", f"{name}.comp")
        if not os.path.isfile(comp_path):
            raise RuntimeError(f"shader listed in {list_path} does not exist: {comp_path}")

    return set(plain_from_list), flow_kernels

def main():
    if len(sys.argv) < 4:
        print("usage: build_shaders.py <glslangValidator> <gpu_dir> <out_shaders_dir> [--gfx11]", file=sys.stderr)
        return 2
    glslang, gpu_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    gfx11 = "--gfx11" in sys.argv[4:]
    os.makedirs(out_dir, exist_ok=True)

    shaders_dir = os.path.join(gpu_dir, "shaders")
    inc_dir = os.path.join(shaders_dir, "include")
    fm_defs = parse_fastmode(os.path.join(gpu_dir, "fastmode.cmake"))
    lt_defs = {k[len("LT_DEFS_"):]: list(v) for k, v in fm_defs.items() if k.startswith("LT_DEFS_")}
    lx_defs = {k[len("LX_DEFS_"):]: list(v) for k, v in fm_defs.items() if k.startswith("LX_DEFS_")}
    glsl_extra = []
    if gfx11:
        glsl_extra = ["-DONR_GFX11=1", "-DFP8QV=0", "-DSPLIT_C=128"]
        strip_defs = {"LT_UP8=1", "LT_MI8=1", "LT_MLP8=1", "LT_MRG8=1", "LT_EMB8=1",
                      "LT_QV8=1", "LT_DFN8=1", "LT_E8RAW=1", "LT_VO8=1", "LT_W1A8=1", "LT_PV8=1", "FP8QV=1",
                      "W2_96UP=1"}
        lt_defs = {k: [x for x in v if x not in strip_defs] for k, v in lt_defs.items()}
        lx_defs = {k: [x for x in v if x not in strip_defs] for k, v in lx_defs.items()}
    plain_runtime, flow_kernels = parse_shader_lists(gpu_dir)

    jobs = []
    def add_spv(out_name, src, extra_args):
        out_path = os.path.join(out_dir, f"{out_name}.spv")
        cmd = [glslang, "-V", "--target-env", "vulkan1.3"] + glsl_extra + extra_args + [f"-I{inc_dir}", "-o", out_path, src]
        jobs.append((out_name, cmd))

    def lite_spv(out_name, src, extra_args):
        add_spv(f"{out_name}_lt", src, extra_args)

    for src in sorted(glob.glob(os.path.join(shaders_dir, "*.comp"))):
        name = os.path.splitext(os.path.basename(src))[0]

        if name in lt_defs:
            ltd = [f"-D{x}" for x in lt_defs[name]]
            if name == "enc0_attr":
                lite_spv("enc0_attr_f", src, ["-DENC_F32=1"] + ltd)
                lite_spv("enc0_attr_f_w", src, ["-DENC_F32=1", "-DATTR_WIDE=1"] + ltd)
                lite_spv("d3d_enc0_attr_f", src, ["-DD3D_GUIDES=1", "-DENC_F32=1"] + ltd)
                lite_spv("d3d_enc0_attr_f_w", src, ["-DD3D_GUIDES=1", "-DENC_F32=1", "-DATTR_WIDE=1"] + ltd)
            elif name in ("dfn_export", "dfn_import", "pooled_history2"):
                lite_spv(name, src, ltd)
            elif name in ("downsample", "downsample_tl"):
                lite_spv(name, src, ltd)
                lite_spv(f"d3d_{name}", src, ["-DD3D_TEX=1", "-DD3D_UAV=1"] + ltd)
            else:
                lite_spv(f"{name}_f", src, ["-DENC_F32=1"] + ltd)
                if name == "enc0_stage_32_64":
                    lite_spv(f"{name}_f_w", src, ["-DENC_F32=1", "-DATTR_WIDE=1"] + ltd)
                if name in flow_kernels:
                    slot = flow_kernels.index(name)
                    fw = 1 if slot > 0 else 0
                    lite_spv(f"{name}_fl", src, ["-DENC_F32=1", "-DFLOW=1", f"-DFLOW_SLOT={slot}", f"-DFLOW_WAIT={fw}"] + ltd)

        if name in lx_defs:  # the experimental level: the same kernels again as _lx (as gpu/CMakeLists.txt)
            lxd = [f"-D{x}" for x in lx_defs[name]]
            add_spv(f"{name}_f_lx", src, ["-DENC_F32=1"] + lxd)
            if name == "enc0_stage_32_64":
                add_spv(f"{name}_f_w_lx", src, ["-DENC_F32=1", "-DATTR_WIDE=1"] + lxd)

        if name in plain_runtime:
            add_spv(name, src, [])

        if re.match(r"^(enc_block_.*_k8|enc_stage_|enc0_stage_|dec_)", name):
            if not name.startswith("enc_block_"):
                fsym = ["-DLT_SYM=1"] if name in ("enc0_stage_32_64", "dec_up_32_64_64", "dec_head_32_64") else []
                add_spv(f"{name}_f", src, ["-DENC_F32=1"] + fsym)
            if name in flow_kernels:
                slot = flow_kernels.index(name)
                fw = 1 if slot > 0 else 0
                add_spv(f"{name}_fl", src, ["-DENC_F32=1", "-DFLOW=1", f"-DFLOW_SLOT={slot}", f"-DFLOW_WAIT={fw}"])

        if name in ("ingest", "pack", "downsample", "downsample_tl"):
            add_spv(f"d3d_{name}", src, ["-DD3D_TEX=1", "-DD3D_UAV=1"])

        if name in ("ae", "enc0_attr"):
            out_n = "d3d_ae" if name == "ae" else "d3d_enc0_attr_f"
            add_spv(out_n, src, ["-DD3D_GUIDES=1", "-DENC_F32=1"])

        if name in ("enc0_attr", "post", "post_dlt"):
            add_spv(f"{name}_f", src, ["-DENC_F32=1"])

        if name in ("enc0_attr", "enc0_stage_32_64"):
            fsymw = ["-DLT_SYM=1"] if name == "enc0_stage_32_64" else []
            add_spv(f"{name}_f_w", src, ["-DENC_F32=1", "-DATTR_WIDE=1"] + fsymw)
            if name == "enc0_attr":
                add_spv("d3d_enc0_attr_f_w", src, ["-DD3D_GUIDES=1", "-DENC_F32=1", "-DATTR_WIDE=1"])

    def run_job(item):
        name, cmd = item
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"shader {name} failed:\n{res.stdout}")

    with concurrent.futures.ThreadPoolExecutor() as ex:
        list(ex.map(run_job, jobs))

    expected = {f"{name}.spv" for name, _ in jobs}
    for spv in glob.glob(os.path.join(out_dir, "*.spv")):
        if os.path.basename(spv) not in expected:
            os.remove(spv)

    print(f"built {len(jobs)} runtime SPIR-V shaders in {out_dir}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
