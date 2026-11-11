"""Compile the enabled cheats of one executable into cheats.bin for bb-probe --patches.

Cheats are "health" and "ammo" (and "rewards": Unlock all rewards). A patch is written only when the executable's
game code is listed and the bytes at its address are the expected ones; anything else is reported and skipped, never
an error.
Addresses are image offsets (virtual addresses of the SELF's load segments).
"""
import hashlib
import re
from pathlib import Path
import struct

import prepare

NOP = '90'
# SHA-1 of the game code: the segments a SELF loads, not its signature header or the metadata a dumper adds at the
# end (launcher GAME_CODE). EU: the original 1.00 build (EU CUSA02343 dumps, US 1.00); US: the 1.02 build (US 1.02
# CUSA02320, EU/Russian CUSA02344).
EU = {'eboot.bin': 'cd749c2f217a842200a9867f7c8af0973c990a74',
      'big2-ps4_Shipping.elf': '1616597c9cf8b4b6d50d7cbf3aeb3677efba6587',
      'big3-ps4_Shipping.elf': '1d949b215bb036fe685511aca4f560d63dd50442'}
US = {'eboot.bin': 'fd024eaeb58c4b778613b54a9e69f6fbc8615a68',
      'big2-ps4_Shipping.elf': 'de8ce6e778f39b3c2914fd71f1df7117282f62e2',
      'big3-ps4_Shipping.elf': 'fed1f511d51613df108101b8a09397beebc70f0c'}

# game code -> cheat -> (address, original bytes, replacement)
PATCHES = {
    # health: jump to the engine's own no-damage path. ammo: the clip decrement is shared with NPC weapons, so U1/U2
    # drop the player Fire's call to it and U3 forces its player-held branch.
    EU['eboot.bin']: {'health': (0x261edb, '7535', 'eb35'),
                      'ammo': (0x71b003, 'e8c8acc2ff', NOP * 5)},
    US['eboot.bin']: {'health': (0x26244b, '7535', 'eb35'),
                      'ammo': (0x730f73, 'e8c854c1ff', NOP * 5)},
    EU['big2-ps4_Shipping.elf']: {'health': (0x35de73, '0f84bc010000', 'e9cf02000090'),
                                  'ammo': (0xa4fd8d, 'e87e8aa6ff', NOP * 5)},
    EU['big3-ps4_Shipping.elf']: {'health': (0x33f87b, '0f84ca020000', 'e96a0b000090'),
                                  'ammo': (0x46a1f9, '4c8d3dc0660501', '41b401eb729090')},
    US['big2-ps4_Shipping.elf']: {'health': (0x7f0143, '0f843c050000', 'e95f06000090'),
                                  'ammo': (0x961ac8, 'e853851800', NOP * 5)},
    US['big3-ps4_Shipping.elf']: {'health': (0x9f9295, '4180bfa7000000007436', 'e9320300009090909090'),
                                  'ammo': (0xd517ac, '4c8d257d92aa0066418b442462', '40b701e9110100009090909090')},
}
# devmode: Naughty Dog's developer mode (the debug menu, the game's own debug text). The check
# "cmp byte [m+A],0 / jne skip / mov cl,[m+B] / test cl,cl / je skip" becomes "mov byte [m+A],0 / jmp / mov byte [m+B],1 /
# jmp": the two flags are set and the developer path runs. On the 1.00 code byte for byte DeathRGH's GoldHEN "Enable
# Debug Menu" (GoldHEN offsets = these + 0x400000); on the 1.02 code the same edit of its check (another register).
DEVMODE = {
    EU['eboot.bin']: (0x2cf8b0, '4180bfe8470000000f85ac0e0000418a8f4448000084c90f849d0e0000',
                      '41c687e847000000eb04ac0e000041c6874448000001eb05849d0e0000'),
    US['eboot.bin']: (0x2cfea0, '4180bfe8470000000f85ac0e0000418a8f4448000084c90f849d0e0000',
                      '41c687e847000000eb04ac0e000041c6874448000001eb05849d0e0000'),
    EU['big2-ps4_Shipping.elf']: (0x14c4fd, '80b921480000000f85c10b00008a898c48000084c90f84b30b0000',
                                  'c6812148000000eb04c10b0000c6818c48000001eb0584b30b0000'),
    US['big2-ps4_Shipping.elf']: (0x3f8f4b, '4180bc2421480000000f85741a0000418a8c248c48000084c90f84641a0000',
                                  '41c684242148000000eb04741a000041c684248c48000001eb0584641a0000'),
    EU['big3-ps4_Shipping.elf']: (0x16218d, '80b9014c0000000f85c10b00008a89fc4b000084c90f84b30b0000',
                                  'c681014c000000eb04c10b0000c681fc4b000001eb0584b30b0000'),
    US['big3-ps4_Shipping.elf']: (0x56cb8b, '4180bc24014c0000000f85551a0000418a8c24fc4b000084c90f84451a0000',
                                  '41c68424014c000000eb04551a000041c68424fc4b000001eb0584451a0000'),
}
for _code, _patch in DEVMODE.items():
    PATCHES[_code]['devmode'] = _patch

# Naughty Dog's debug-menu switches, kept set by the runtime (BB_KEEP_BYTES): bytes in memory the game creates, so no
# patch of the file reaches them. Uncharted 3's menu has "No Reload" and "One Hit Kills" in its cheat block (Infinite
# Ammo +0x61, Infinite Grenades +0x62, No Reload +0x63, One Hit Kills +0x164); Uncharted 2 reads no-reload from the
# byte after its Infinite Ammo switch (+0x3b; no menu item). 1.02 addresses by BilalReloaded; the EU 1.00 ones found
# the same way (the menu items' base + offset).
KEEP = {
    US['big2-ps4_Shipping.elf']: {'noreload': [0x104f84b]},
    EU['big2-ps4_Shipping.elf']: {'noreload': [0xe654bb]},
    US['big3-ps4_Shipping.elf']: {'noreload': [0x17faa93], 'onehit': [0x17fab94]},
    EU['big3-ps4_Shipping.elf']: {'noreload': [0x14c0923], 'onehit': [0x14c0a24]},
}


# Unlock all rewards (BB_UNLOCK_REWARDS=1, launcher Game > Unlock all rewards; run.py adds 'rewards' to every
# game's list). From BilalReloaded's Uncharted port (tools/probe_uncharted.py REWARD_POKES / REWARD_PATCHES,
# GPL-2.0-or-later, made for 1.02); the 1.00 sites found the same way. Nothing is written to the save: turned off,
# the Bonuses menus show the player's own progress again; treasures, medals and trophies are not touched.
#
# Uncharted 2 and 3: Naughty Dog's debug-menu switches "Give all Store Items" and "Give All Movies" (the menu is
# unreachable), kept set by the runtime: every reward and story cutscene reads as unlocked while they are set. Each
# is checked against the menu item that points at it: (offset, address of that rip-relative lea, its bytes).
REWARD_SWITCHES = {
    US['big2-ps4_Shipping.elf']: [(0x1114178, 0x3fed99, '488d05d853d100'), (0x1114179, 0x40007f, '488d05f340d100')],
    EU['big2-ps4_Shipping.elf']: [(0xeb67f0, 0x14f1a9, '488d054076d600'), (0xeb67f1, 0x14f570, '488d0d7a72d600')],
    US['big3-ps4_Shipping.elf']: [(0x17b2a94, 0x4bae0b, '488d05827c2f01'), (0x17b2a95, 0x4bb57b, '488d0513752f01')],
    EU['big3-ps4_Shipping.elf']: [(0x1429524, 0x100763, '488d15ba8d3201'), (0x1429525, 0x1008b1, '488d156d8c3201')],
}
# Code patches (address, original, replacement; one byte changes in each). Uncharted 2 and 3 let some rewards
# (tweaks, render modes) be enabled only once the game was finished on that difficulty: the reward screen's item
# info sets "needs the game finished" (+0xe) to 1 before clearing it from the save's finished-difficulty bits; it is
# set to 0. Drake's Fortune keeps each reward's status in its own record (bit 0 unlocked) and its debug switch writes
# them, so its checks are patched instead: every reward reads as unlocked, usable and counted, can be enabled
# without finishing the game first, and the Story Cutscenes list skips no chapter (the status bytes are untouched).
_U1_REWARDS = (
    # IsUnlocked(item): and al, 1 -> mov al, 1
    ('item check', 'b001803f00750d4885f674098a86880000002401', 'b001803f00750d4885f674098a8688000000b001'),
    # IsUnlocked(category, id): the same
    ('id check', '75134d85e4740c418a8424880000002401eb02', '75134d85e4740c418a842488000000b001eb02'),
    # the "Unlocked N of 47" counter: je skip -> je +0
    ('counter', '4c89f275154885c07422f68088000000017419', '4c89f275154885c07422f68088000000017400'),
    # usable-by-id check: je not -> je +0
    ('usable by id', '41803e00750af684088800000001741e488d55d4', '41803e00750af6840888000000017400488d55d4'),
    # CanEnable: test [item+0x88], 1; sete dl (dl = locked) -> setb dl (test clears CF)
    ('enable check', '41f68688000000010f94c24584ff0f95c184c0', '41f68688000000010f92c24584ff0f95c184c0'),
    # CanEnable's "finish the game on this difficulty first": mov r13b, 1 -> mov r13b, 0
    ('finished first', '448bb87848000041b5014183ff067338', '448bb87848000041b5004183ff067338'),
    # Story Cutscenes list: cmp [node+0x50], 0; js skip (chapter not reached) -> js +0
    ('story cutscenes', '488b7840837f50000f88880000004c8b4210', '488b7840837f50000f88000000004c8b4210'),
)
REWARD_PATCHES = {
    US['eboot.bin']: [(label, address, original, replacement) for address, (label, original, replacement) in
                      zip((0x32c330, 0x32c3c8, 0x32c506, 0x32cd44, 0x32c8a8, 0x32c80f, 0x138aad), _U1_REWARDS)],
    EU['eboot.bin']: [(label, address, original, replacement) for address, (label, original, replacement) in
                      zip((0x32bbb0, 0x32bc48, 0x32bd86, 0x32c5c4, 0x32c128, 0x32c08f, 0x13882d), _U1_REWARDS)],
    US['big2-ps4_Shipping.elf']: [('finished first', 0xa4ca2f, '7441c6430e01833d401fa800', '7441c6430e00833d401fa800')],
    EU['big2-ps4_Shipping.elf']: [('finished first', 0x430484, '753641c6450e01e8304bd9ff', '753641c6450e00e8304bd9ff')],
    US['big3-ps4_Shipping.elf']: [('finished first', 0x4bd550, '74054c39f0725c41c6450e01488d05',
                                   '74054c39f0725c41c6450e00488d05')],
    EU['big3-ps4_Shipping.elf']: [('finished first', 0x101f65, '754541c6450e01488d3d65e02f01',
                                   '754541c6450e00488d3d65e02f01')],
}


def reward_switches(data, code):
    """Offsets of the 'give all' switches of this build whose menu items point at them; the rest are reported."""
    switches = REWARD_SWITCHES.get(code, [])
    if not switches:
        return []
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    offsets = []
    for offset, lea, encoding in switches:
        encoding = bytes.fromhex(encoding)
        target = lea + len(encoding) + struct.unpack_from('<i', encoding, 3)[0]
        if image_bytes(elf, program_headers, lea, len(encoding)) != encoding or target != offset:
            print(f'Unlock all rewards: switch {offset:#x} skipped, unexpected code at {lea:#x}')
            continue
        offsets.append(offset)
    return offsets


# Extra quality (launcher Display: Full resolution textures, Hi-res lamp and flashlight shadows; BB_FULL_RES_TEXTURES=1,
# BB_HIRES_SHADOWS=1): Uncharted 2 and 3's debug-menu render switches "Force Full Resolution Textures" (every texture at its full
# size, none of the streamed smaller versions; off on the PS4) and "Enable Hires Local Shadow Buffer" (a bigger shadow map for
# lamps and the flashlight), kept set like the cheats (BilalReloaded's QUALITY_POKES: render-settings block + 0x7c0 / + 0x1c4,
# measured ~1% / ~0% FPS). Found from the menu items in every build (his addresses are US 1.02's): the full-resolution item
# loads the block (lea reg, [rip + block]) and stores the switch's address (lea rax, [reg + 0x7c0]); the shadow item's
# address is lea rax, [r12 + 0x1c4] with the same block in r12.
# lea reg, [rip + disp32] (48 or 4c, 8d, a ModRM with mod 00 and r/m 101)
LEA_RIP = re.compile(b'(?=' + re.escape(bytes([0x48])).join([b'(?:', b'|' + re.escape(bytes([0x4c])) + b')']) + re.escape(bytes([0x8d])) +
                     b'[' + b''.join(re.escape(bytes([m])) for m in (0x05, 0x0d, 0x15, 0x1d, 0x25, 0x2d, 0x35, 0x3d)) + b'])', re.S)   # (a lookahead: overlapping candidates too)
QUALITY_LABELS = {'fullres': b'Force Full Resolution Textures', 'hiresshadow': b'Enable Hires Local Shadow Buffer'}
# Depth of field off (launcher Depth of field: Off everywhere, BB_DOF=0): the engines' own "Enable Depth of Field" switch in the
# same block (+ 0x1bd, BilalReloaded's no_dof) kept at 0: the frame stays full resolution (skipping the blur shaders alone left the
# composite mixing in the half-size picture where the focus mask is set: soft, shimmering distance). Not a menu label of its own.
QUALITY_EXTRA = {'nodof': (0x1bd, 0)}


def quality_switches(data, names):
    """{name: image offset} of the extra-quality switches asked for whose menu code is as expected; the rest reported."""
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    loads = [p for p in program_headers if p['type'] in (1, 0x61000010) and p['filesz']]
    def address_of(text):
        for p in loads:
            at = bytes(elf[p['offset']:p['offset'] + p['filesz']]).find(text + bytes(1))
            if at >= 0:
                return p['vaddr'] + at
        return None
    code = next((p for p in loads if p['flags'] & 1), None)
    if code is None:
        return {}
    text = bytes(elf[code['offset']:code['offset'] + code['filesz']])
    leas = {}   # rip-relative lea targets -> their offsets (one pass over the code)
    for m in LEA_RIP.finditer(text):
        i = m.start()
        if i + 7 <= len(text):
            leas.setdefault(code['vaddr'] + i + 7 + struct.unpack_from('<i', text, i + 3)[0], []).append(i)
    def label_leas(target):
        return leas.get(target, [])
    result = {}
    full = address_of(QUALITY_LABELS['fullres'])
    block = None
    for i in label_leas(full) if full is not None else []:
        window = text[i:i + 0x60]
        # lea r13, [rip + block] (4c 8d 2d) followed by lea rax, [r13 + 0x7c0] (49 8d 85 c0 07 00 00)
        j = window.find(bytes.fromhex('498d85c0070000'))
        k = window.find(bytes.fromhex('4c8d2d'))
        if j > 0 and 0 <= k < j:
            block = code['vaddr'] + i + k + 7 + struct.unpack_from('<i', window, k + 3)[0]
            break
    if block is None:
        # Uncharted 3 1.02 (EU and US share the code): r13 holds the block from further up; BilalReloaded's address 0x18c6280,
        # taken when this build's full-resolution item stores [r13 + 0x7c0] right after its label
        if full is not None and any(text[i:i + 0x60].find(bytes.fromhex('498d85c0070000')) > 0 for i in label_leas(full)) and                 any(code['vaddr'] + i == 0xe127f1 for i in label_leas(full)):
            block = 0x18c6280
    if block is None:
        # 1.00 (EU and US): the item's constructor gets the switch's address in rdx right before the label,
        # lea rdx, [r12-r15 + 0x7c0 / 0x1c4], and that register was loaded with the block (lea reg, [rip + block]) earlier
        # in the same function: both items must give the same block
        def block_1_00(label, offset):
            if label is None:
                return None
            for i in label_leas(label):
                for reg, modrm, size in ((12, bytes.fromhex('9424'), 8), (13, bytes.fromhex('95'), 7), (14, bytes.fromhex('96'), 7), (15, bytes.fromhex('97'), 7)):
                    lea_rdx = bytes.fromhex('498d') + modrm + struct.pack('<i', offset)
                    if text[i - size:i] != lea_rdx:
                        continue
                    load = bytes((0x4c, 0x8d, ((reg - 8) << 3) | 5))
                    j = text.rfind(load, max(0, i - 0x4000), i - size)
                    if j >= 0:
                        return code['vaddr'] + j + 7 + struct.unpack_from('<i', text, j + 3)[0]
            return None
        a = block_1_00(full, 0x7c0)
        b = block_1_00(address_of(QUALITY_LABELS['hiresshadow']), 0x1c4)
        if a is not None and a == b:
            block = a
            result['_100'] = True
    if block is None:
        print('Extra quality: the render-settings block was not found in this build; not applied')
        return {}
    if 'fullres' in names:
        result['fullres'] = block + 0x7c0
    for name, (offset, _value) in QUALITY_EXTRA.items():
        if name in names:
            result[name] = block + offset
    if result.pop('_100', False):   # (both switches already checked against their items)
        if 'hiresshadow' in names:
            result['hiresshadow'] = block + 0x1c4
        return result
    if 'hiresshadow' in names:
        shadow = address_of(QUALITY_LABELS['hiresshadow'])
        ok = any(text[i:i + 0x50].find(bytes.fromhex('498d8424c4010000')) > 0 for i in label_leas(shadow)) if shadow else False
        if ok:
            result['hiresshadow'] = block + 0x1c4
        else:
            print('Extra quality: the hi-res shadow switch was not found in this build; not applied')
    return result


def keep_bytes(path, cheats):
    """BB_KEEP_BYTES ("offset=1,...") for the enabled switch cheats of one executable; '' when none applies."""
    if not any(c in ('noreload', 'onehit', 'rewards', 'fullres', 'hiresshadow', 'nodof') for c in cheats):
        return ''
    try:
        data = Path(path).read_bytes()
        code = code_hash(data)
        table = KEEP.get(code, {})
        rewards = reward_switches(data, code) if 'rewards' in cheats else []
    except (OSError, ValueError, KeyError, IndexError, struct.error) as error:
        print(f'Cheats: switches not kept ({error})')
        return ''
    quality = {}
    if any(c in QUALITY_LABELS or c in QUALITY_EXTRA for c in cheats):
        try:   # (on its own: a surprise here must not drop the cheats and rewards)
            quality = quality_switches(data, [c for c in cheats if c in QUALITY_LABELS or c in QUALITY_EXTRA])
        except (ValueError, KeyError, IndexError, struct.error) as error:
            print(f'Extra quality: not applied ({error})')
    keep = []
    for name, offset in quality.items():
        keep.append(f'{offset:x}={QUALITY_EXTRA[name][1] if name in QUALITY_EXTRA else 1}')
        what = {'fullres': 'full resolution textures on', 'hiresshadow': 'hi-res local shadows on', 'nodof': 'depth of field off (the engine switch)'}
        print(f'Extra quality: {what.get(name, name)} ({offset:#x})')
    for name in cheats:
        for offset in table.get(name, []):
            keep.append(f'{offset:x}=1')
            print(f'Cheats: {name} on')
    if rewards:
        keep += [f'{offset:x}=1' for offset in rewards]
        print(f'Unlock all rewards: {len(rewards)} give-all switches kept set')
    return ','.join(keep)


def code_hash(data):
    """SHA-1 of a SELF's loaded segments, or of the whole file when it is not a SELF."""
    digest = hashlib.sha1()
    if data[:4] == b'\x4f\x15\x3d\x1d' and len(data) >= 0x20:
        count, = struct.unpack_from('<H', data, 0x18)
        for i in range(count):
            props, offset, size, _memsz = struct.unpack_from('<QQQQ', data, 0x20 + 32 * i)
            if props & 0x800:   # a segment with data in the file
                digest.update(data[offset:offset + size])
    else:
        digest.update(data)
    return digest.hexdigest()


def image_bytes(elf, program_headers, address, size):
    for p in program_headers:
        if p['type'] in (1, 0x61000010) and p['vaddr'] <= address and address + size <= p['vaddr'] + p['filesz']:
            start = p['offset'] + address - p['vaddr']
            return bytes(elf[start:start + size])
    return None


def compile_cheats(path, cheats):
    """[(address, replacement bytes)] for the enabled cheats; reasons go to stdout."""
    data = Path(path).read_bytes()
    code = code_hash(data)
    table = PATCHES.get(code)
    if table is None:
        print(f'Cheats: {Path(path).name} is not a supported version; none applied')
        if 'rewards' in cheats:
            print(f'Unlock all rewards: {Path(path).name} is not a supported version; not applied')
        return []
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    writes = []
    if 'rewards' in cheats:
        applied = []
        for label, address, original, replacement in REWARD_PATCHES.get(code, []):
            original, replacement = bytes.fromhex(original), bytes.fromhex(replacement)
            if image_bytes(elf, program_headers, address, len(original)) != original:
                print(f'Unlock all rewards: {label} skipped, unexpected code at {address:#x}')
                continue
            writes.append((address, replacement))
            applied.append(label)
        if applied:
            print(f'Unlock all rewards: {", ".join(applied)} patched')
    for name in cheats:
        if name not in table:
            continue
        address, original, replacement = table[name]
        original, replacement = bytes.fromhex(original), bytes.fromhex(replacement)
        if image_bytes(elf, program_headers, address, len(original)) != original:
            print(f'Cheats: {name} skipped, unexpected code at {address:#x}')
            continue
        writes.append((address, replacement))
        print(f'Cheats: {name} on')
    return writes


def write_patch_file(out, writes):
    with open(out, 'wb') as f:
        f.write(b'BBPATCH2' + struct.pack('<QQ', 0, len(writes)))
        for address, data in writes:
            f.write(struct.pack('<QQ', address, len(data)) + data)


def build(path, out_dir, cheats):
    """Path of out_dir/cheats.bin for the enabled cheats, or None when nothing applies."""
    out = Path(out_dir) / 'cheats.bin'
    try:
        out.unlink(missing_ok=True)
        writes = compile_cheats(path, cheats) if cheats else []
        if writes:
            write_patch_file(out, writes)
            return out
    except (OSError, ValueError, KeyError, IndexError) as error:
        print(f'Cheats: not applied ({error})')
    return None
