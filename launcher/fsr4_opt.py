"""FSR 4 v07: download of the third-party model files and the port's faster/fixed passes built from them.

The FSR 4 v07 files are not part of the port's package. The launcher downloads them on the player's request from
the third party that published them (FireBurn/Q2RTX on GitHub) and then builds fsr4_shaders/opt/ here, on the
player's machine, with the open-source spirv-cross and glslangValidator shipped in bin/ (same result as
tools/fsr4_optimize.sh, which the Perl scripts in tools/ do for developers):
  *_post.spv    stores through shared memory, bit-exact, ~3.5x faster (tools/fsr4_post_lds.pl);
  *_pass11.spv  no out-of-bounds writes (a data race at the 1080 tier; tools/fsr4_pass11_guard.pl).
vk_fsr4.cpp prefers the files in opt/ and uses the originals for anything missing there.
"""
import re
import subprocess
import tempfile
import urllib.request
from pathlib import Path

FSR4_COMMIT = 'ae8d628fae208813172446d1e49ed94150b04658'
FSR4_BASE = f'https://raw.githubusercontent.com/FireBurn/Q2RTX/{FSR4_COMMIT}/baseq2/fsr4_shaders'
MODELS = ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs')
TIERS = ('1080', '2160')
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def files():
    """The FSR 4 v07 set of tools/fetch_fsr4_assets.sh (1080 and 2160 tiers)."""
    names = ['LICENSE-FSR4-v07.txt', 'rcas.spv', 'spd_auto_exposure.spv']
    for model in MODELS:
        names += [f'fsr4_model_v07_i8_{model}_initializers.bin', f'fsr4_model_v07_i8_{model}_pre_weights.bin',
                  f'fsr4_model_v07_i8_{model}_shader_manifest.json']
        for tier in TIERS:
            names += [f'fsr4_model_v07_i8_{model}_{tier}_pre.spv', f'fsr4_model_v07_i8_{model}_{tier}_post.spv']
            names += [f'fsr4_model_v07_i8_{model}_{tier}_pass{n}.spv' for n in range(1, 13)]
    return names


def missing(folder):
    folder = Path(folder)
    return [name for name in files() if not (folder / name).is_file() or not (folder / name).stat().st_size]


def opt_jobs():
    """(original, rewrite, entry point) of every pass built into opt/."""
    jobs = []
    for model in MODELS:
        for tier in TIERS:
            jobs.append((f'fsr4_model_v07_i8_{model}_{tier}_post.spv', post_lds, 'main'))
            jobs.append((f'fsr4_model_v07_i8_{model}_{tier}_pass11.spv', pass11_guard, 'fsr4_model_v07_i8_pass11'))
    return jobs


def opt_missing(folder):
    opt = Path(folder) / 'opt'
    return [job for job in opt_jobs() if not (opt / job[0]).is_file() or not (opt / job[0]).stat().st_size]


def download(folder, progress=None):
    """Downloads the missing files into folder; progress(done, total) after each one."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    todo = missing(folder)
    for done, name in enumerate(todo, 1):
        part = folder / (name + '.part')
        with urllib.request.urlopen(f'{FSR4_BASE}/{name}', timeout=60) as response:
            part.write_bytes(response.read())
        part.replace(folder / name)
        if progress:
            progress(done, len(todo))


# --- the rewrites (Python versions of tools/Fsr4SpirvCrossFixes.pm, fsr4_pass11_guard.pl, fsr4_post_lds.pl) ---

SIGNED_UNPACK = '''// bbport: signed int8 unpack (spirv-cross emits the unsigned unpack8(uint)).
i8vec4 bbUnpackS8(uint v)
{
    return unpack8(int(v));
}

'''


def signed_unpack(src):
    """spirv-cross translates OpBitcast uint -> v4char (signed int8) as unpack8(uint), which returns an unsigned
    u8vec4. The passes only ever unpack signed int8 tensors, so every unpack8 goes through a signed helper."""
    src, n = re.subn(r'\bunpack8\(', 'bbUnpackS8(', src)
    if not n:
        raise ValueError('expected signed int8 unpacks')
    src, n = re.subn(r'^(void main\(\)\n)', lambda m: SIGNED_UNPACK + m.group(1), src, count=1, flags=re.M)
    if not n:
        raise ValueError('no main')
    return src


def pass11_guard(src):
    """Model pass 11 (decoder, 1/4 -> 1/2 resolution): invocations past the input width (the dispatch rounds the
    width up to 64) write the first pixels of the next output row, racing with their real writers; they return
    first. The input size comes from the pass's own neighbour bounds check."""
    m = re.search(r'lessThan\(uvec3\(_\d+\), uvec3\((\d+)u, (\d+)u, 32u\)\)', src)
    if not m:
        raise ValueError('no input bounds check')
    w, h = m.groups()
    if re.search(r'\bbarrier\(|\bshared\b', src):
        raise ValueError('pass uses shared memory or barriers')
    src = signed_unpack(src)
    guard = ('void main()\n{\n'
             '    // bbport: invocations past the input width would write the next row\'s first pixels.\n'
             f'    if (gl_GlobalInvocationID.x >= {w}u || gl_GlobalInvocationID.y >= {h}u)\n'
             '    {\n        return;\n    }\n')
    src, n = re.subn(r'^void main\(\)\n\{\n', lambda _: guard, src, count=1, flags=re.M)
    if not n:
        raise ValueError('no main')
    return src


POST_DECL = '''// bbport: the workgroup's 16x16 output block, stored in contiguous rows after the loop.
shared vec4 bb_rec[256];
shared vec4 bb_hist[256];
shared vec4 bb_out[256];
uint bbLocal(uvec2 p)
{
    uvec2 l = p - gl_WorkGroupID.xy * 16u;
    return l.y * 16u + l.x;
}

void main()
'''
POST_FLUSH = '''    barrier();
    uvec2 bbBase = gl_WorkGroupID.xy * 16u;
    for (uint bbK = 0u; bbK < 4u; bbK++)
    {
        uint bbI = gl_LocalInvocationIndex + 64u * bbK;
        ivec2 bbP = ivec2(bbBase + uvec2(bbI % 16u, bbI / 16u));
        imageStore(rw_recurrent_0, bbP, bb_rec[bbI]);
        imageStore(rw_history_color, bbP, bb_hist[bbI]);
        imageStore(rw_mlsr_output_color, bbP, bb_out[bbI]);
    }
}
'''


def post_lds(src):
    """Post pass: the 8x8 workgroup collects its 16x16 output block in shared memory and stores it in contiguous
    rows (bit-exact; values stay float in shared memory and are converted by the store as before)."""
    if 'layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;' not in src:
        raise ValueError('unexpected local size')
    src = signed_unpack(src)
    slots = {'rw_recurrent_0': ('bb_rec', 'vec4'), 'rw_history_color': ('bb_hist', 'vec4'),
             'rw_mlsr_output_color': ('bb_out', 'vec4')}
    for image in sorted(slots):
        array, kind = slots[image]
        src, n = re.subn(rf'imageStore\({image}, ivec2\((_\d+)\), ',
                         lambda m, a=array, k=kind: f'{a}[bbLocal({m.group(1)})] = {k}(', src)
        if n != 1:
            raise ValueError(f'expected one store to {image}, found {n}')
    src, n = re.subn(r'^void main\(\)\n', lambda _: POST_DECL, src, count=1, flags=re.M)
    if not n:
        raise ValueError('no main')
    # main is the last function: its closing brace ends the file.
    src, n = re.subn(r'\n}\s*\Z', lambda _: '\n' + POST_FLUSH, src, count=1)
    if not n:
        raise ValueError('no end of main')
    return src


def build_opt(folder, tools, progress=None):
    """Builds the missing opt/ passes from the originals in folder with spirv-cross and glslangValidator from
    the tools folder; progress(done, total) after each one. Returns the number built."""
    folder, tools = Path(folder), Path(tools)
    opt = folder / 'opt'
    opt.mkdir(parents=True, exist_ok=True)
    todo = opt_missing(folder)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for done, (name, rewrite, entry) in enumerate(todo, 1):
            subprocess.run([str(tools / 'spirv-cross.exe'), str(folder / name), '--vulkan-semantics', '--entry',
                            entry, '--output', str(tmp / 'in.comp')], check=True, capture_output=True,
                           creationflags=NO_WINDOW)
            # Windows builds of spirv-cross write CRLF; the rewrites match LF lines.
            text = (tmp / 'in.comp').read_bytes().decode('utf-8').replace('\r', '')
            (tmp / 'out.comp').write_bytes(rewrite(text).encode('utf-8'))
            subprocess.run([str(tools / 'glslangValidator.exe'), '-V', '--target-env', 'vulkan1.3', '-S', 'comp',
                            '-e', entry, '--source-entrypoint', 'main', str(tmp / 'out.comp'), '-o',
                            str(tmp / 'out.spv')], check=True, capture_output=True, creationflags=NO_WINDOW)
            (tmp / 'out.spv').replace(opt / name)
            if progress:
                progress(done, len(todo))
    return len(todo)


if __name__ == '__main__':
    # python fsr4_opt.py <fsr4_shaders folder> <tools folder>: builds opt/ (for checks against fsr4_optimize.sh)
    import sys
    print(build_opt(sys.argv[1], sys.argv[2]), 'built')
