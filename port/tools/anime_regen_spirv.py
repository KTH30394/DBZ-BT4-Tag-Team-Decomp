#!/usr/bin/env python3
"""Regenerate the checked-in SPIR-V of every shader whose source changed (the anime pass changes gs.frag and
vu0.vert, and so gsnz.frag / gswb.frag, which undefined.py makes from gs.frag), update generated/spirv/manifest.json,
and leave every .spv plus gsnz.frag / gswb.frag in port/build/gen/gs for dx12_shaders.py and d3d9_shaders.py.
Needs glslangValidator (GLSLANG_VALIDATOR or on PATH)."""
import json, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port/tools'))
import shader_spirv

gen = ROOT / 'port/build/gen/gs'
gen.mkdir(parents=True, exist_ok=True)
cache = ROOT / 'port/src/gs/generated/spirv'
manifest_path = cache / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
glslang = os.environ.get('GLSLANG_VALIDATOR') or shutil.which('glslangValidator')
if not glslang:
    sys.exit('glslangValidator not found')

frag = (ROOT / 'port/src/gs/shaders/gs.frag').read_text()
(gen / 'gsnz.frag').write_text(frag.replace('\n', '\n#define NO_FRAG_DEPTH\n', 1))
(gen / 'gswb.frag').write_text(frag.replace('\n', '\n#define NO_FRAG_DEPTH\n#define GS_BLOCK\n', 1))
sources = sorted((ROOT / 'port/src/gs/shaders').glob('*.*')) + [gen / 'gsnz.frag', gen / 'gswb.frag']
changed = []
for src in sources:
    if src.suffix not in ('.vert', '.frag'):
        continue
    h = shader_spirv.source_hash(src.read_bytes())
    entry = manifest.get(src.name)
    spv = cache / (src.name + '.spv')
    if entry is None or entry['source_sha256'] != h or not spv.exists():
        r = subprocess.run([glslang, '-V', '-S', src.suffix[1:], str(src), '-o', str(spv)], capture_output=True, text=True)
        if r.returncode:
            sys.exit('FAILED ' + src.name + ':\n' + r.stdout + r.stderr)
        import hashlib
        manifest[src.name] = {'source_sha256': h, 'spv_sha256': hashlib.sha256(spv.read_bytes()).hexdigest()}
        changed.append(src.name)
    shutil.copyfile(spv, gen / (src.name + '.spv'))
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
print('SPIR-V regenerated:', ', '.join(changed) if changed else 'none')
