"""Compile the enabled cheats of one executable into cheats.bin for bb-probe --patches.

Cheats are "health" and "ammo". A patch is written only when the executable's game code is listed and the bytes at
its address are the expected ones; anything else is reported and skipped, never an error.
Addresses are image offsets (virtual addresses of the SELF's load segments).
"""
import hashlib
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


def keep_bytes(path, cheats):
    """BB_KEEP_BYTES ("offset=1,...") for the enabled switch cheats of one executable; '' when none applies."""
    if not any(c in ('noreload', 'onehit') for c in cheats):
        return ''
    try:
        table = KEEP.get(code_hash(Path(path).read_bytes()), {})
    except OSError:
        return ''
    keep = []
    for name in cheats:
        for offset in table.get(name, []):
            keep.append(f'{offset:x}=1')
            print(f'Cheats: {name} on')
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
    table = PATCHES.get(code_hash(data))
    if table is None:
        print(f'Cheats: {Path(path).name} is not a supported version; none applied')
        return []
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    writes = []
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
