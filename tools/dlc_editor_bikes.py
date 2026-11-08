"""Let the editor's track settings allow the DLC bikes (Gecko 520, Banshee).

The game sends the editor every bike in its bike table, DLC bikes included (gameMode.allowedBikes:
id, name, enabled per bike; built at 0x6b6610), but the PC track settings menu
(EditorTrackSettingsMenuPC.swf, class trialsFmx.EditorTrackSettingsMenuPC) only makes checkboxes for
entries 0-4. A track's allowed-bike mask could never include bikes 5 and 6, so DLC-bike tracks could
be ridden in the editor but not published with those bikes (Reddit report, 2026-11-07).

This patches the class's ActionScript 2 bytecode in the player's own data_misc.pak (in GameEntry.swf,
whose copy of the class is the one the game uses, and in the menu's own file):
  - generate_gamemode_groups: two more checkboxes, gameMode.allowedBikes.5 and .6;
  - skipGroupItem: in the allowed-bikes page, an entry the game didn't send (no DLC installed) is
    skipped, so players without the DLC see the same five checkboxes as before.
Branch offsets and function sizes that span an insertion are adjusted. Running it again changes
nothing. Backup: data_misc.pak.before-editor-bikes (once).

Usage: python dlc_editor_bikes.py GAME_FOLDER
"""
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of, NAME_TABLE            # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

# GameEntry.swf (loaded first) carries its own copy of the class; AS2 keeps the first definition,
# so that copy is the one the editor runs. The menu's own file is patched too.
MENUS = ('GameEntry.swf', 'EditorTrackSettingsMenuPC.swf')
LAST = b'gameMode.allowedBikes.4\x00'
EXTRA = (5, 6)
BACKUP = '.before-editor-bikes'


# ---- AS2 actions ----
def actions(code, start=0, end=None):
    """[(offset, action code, payload)] for a flat action stream (function bodies included)."""
    out, i = [], start
    end = len(code) if end is None else end
    while i < end:
        a = code[i]
        if a == 0:
            out.append((i, 0, b''))
            i += 1
            continue
        if a >= 0x80:
            n = struct.unpack_from('<H', code, i + 1)[0]
            out.append((i, a, code[i + 3:i + 3 + n]))
            i += 3 + n
        else:
            out.append((i, a, b''))
            i += 1
    return out


def action(a, payload=b''):
    return bytes([a]) + (struct.pack('<H', len(payload)) + payload if a >= 0x80 else b'')


def constant_pool(code):
    for _, a, p in actions(code):
        if a == 0x88:
            n = struct.unpack_from('<H', p)[0]
            return p[2:].split(b'\x00')[:n]
    raise ValueError('no constant pool')


def push_values(p):
    """Decode a Push payload into [(type, raw bytes)]."""
    out, i = [], 0
    sizes = {1: 4, 4: 1, 5: 1, 6: 8, 7: 4, 8: 1, 9: 2}
    while i < len(p):
        t = p[i]
        if t == 0:
            j = p.index(b'\x00', i + 1)
            out.append((0, p[i + 1:j + 1]))
            i = j + 1
        elif t in (2, 3):
            out.append((t, b''))
            i += 1
        else:
            out.append((t, p[i + 1:i + 1 + sizes[t]]))
            i += 1 + sizes[t]
    return out


def push(*values):
    """Push action: ('c', pool index), ('reg', n), int, bool."""
    out = b''
    for v in values:
        if isinstance(v, tuple) and v[0] == 'reg':
            out += bytes([4, v[1]])
        elif isinstance(v, tuple):
            out += bytes([8, v[1]]) if v[1] < 256 else bytes([9]) + struct.pack('<H', v[1])
        elif isinstance(v, bool):
            out += bytes([5, int(v)])
        else:
            out += bytes([7]) + struct.pack('<i', v)
    return action(0x96, out)


def with_constants(code, extra):
    """The stream with `extra` strings appended to its leading ConstantPool; their indices."""
    assert code[0] == 0x88
    size = struct.unpack_from('<H', code, 1)[0]
    count = struct.unpack_from('<H', code, 3)[0]
    payload = code[3:3 + size] + b''.join(e + bytes(1) for e in extra)
    payload = struct.pack('<H', count + len(extra)) + payload[2:]
    return action(0x88, payload) + code[3 + size:], list(range(count, count + len(extra)))


def pool_index(pool, text):
    return pool.index(text.encode('latin-1'))


def fix_spans(code, at, size):
    """Adjust function sizes and branch offsets around `size` bytes inserted at offset `at`."""
    code = bytearray(code)
    for o, a, p in actions(code):
        if a in (0x9B, 0x8E):                         # DefineFunction / DefineFunction2
            body = o + 3 + len(p)
            size_at = o + 3 + len(p) - 2
            n = struct.unpack_from('<H', code, size_at)[0]
            if body <= at < body + n:
                struct.pack_into('<H', code, size_at, n + size)
        elif a in (0x99, 0x9D):                       # Jump / If
            e = o + 5
            off = struct.unpack_from('<h', p)[0]
            t = e + off
            if off >= 0 and e <= at <= t and not (at == t == e):
                struct.pack_into('<h', code, o + 3, off + size)
            elif off < 0 and t < at < e:
                struct.pack_into('<h', code, o + 3, off - size)
        elif a in (0x8F, 0x94):                       # Try / With: none expected around our edits
            raise ValueError('unexpected Try/With block')
    return bytes(code)


def insert(code, at, blob):
    code = fix_spans(code, at, len(blob))
    return code[:at] + blob + code[at:]


def function_body(code, name_pool_idx):
    """Start offset of the body of the method stored under the constant `name_pool_idx`."""
    acts = actions(code)
    for k, (o, a, p) in enumerate(acts):
        if a == 0x8E and k:
            po, pa, pp = acts[k - 1]
            vals = push_values(pp) if pa == 0x96 else []
            if vals and vals[-1][0] in (8, 9) and int.from_bytes(vals[-1][1], 'little') == name_pool_idx:
                return o + 3 + len(p)
    raise ValueError('method not found')


def patch_class(code):
    """The class's action stream with the two checkboxes and the skip rule added (None: already).
    The class sits inside one long If whose s16 offset is ~120 bytes from its limit, so the added
    code is kept small: new strings go into the constant pool (before that If), and the checkboxes
    pass propCheckbox's value "0" directly."""
    pool = constant_pool(code)
    if b'gameMode.allowedBikes.5' in pool:
        return None
    c = {k: pool_index(pool, k) for k in ('', 'push_item', 'gameMode.allowedBikes', 'identify',
                                          'findSettingData', 'gameMode.allowedBikes.4', 'skipGroupItem')}
    acts = actions(code)
    for k, (o, a, p) in enumerate(acts):
        if a == 0x96 and push_values(p)[:1] in ([(8, bytes([c['gameMode.allowedBikes.4']]))],
                                                [(9, struct.pack('<H', c['gameMode.allowedBikes.4']))]):
            break
    else:
        raise ValueError('allowedBikes.4 checkbox not found')
    assert acts[k + 1][1] == 0x52 and acts[k + 2][1] == 0x17, 'unexpected checkbox code'   # CallMethod, Pop
    block_end = acts[k + 2][0] + 1
    zero, five, six = len(pool), len(pool) + 1, len(pool) + 2     # appended to the pool below
    blob = b''
    for ident in (five, six):
        blob += push(('c', c['']), ('c', c['']), ('c', zero), ('c', ident), ('reg', 2), 5, ('reg', 1),
                     ('c', c['push_item'])) + action(0x52) + action(0x17)
    code = insert(code, block_end, blob)
    # skipGroupItem: if (groupId == "gameMode.allowedBikes" && !this.findSettingData(item.identify)) return true;
    body = function_body(code, c['skipGroupItem'])
    tail = push(True) + action(0x3E)
    second = (push(('reg', 3), ('c', c['identify'])) + action(0x4E) +
              push(1, ('reg', 1), ('c', c['findSettingData'])) + action(0x52))
    second += action(0x9D, struct.pack('<h', len(tail)))
    first = push(('reg', 2), ('c', c['gameMode.allowedBikes'])) + action(0x49) + action(0x12)
    first += action(0x9D, struct.pack('<h', len(second) + len(tail)))
    code = insert(code, body, first + second + tail)
    code, idx = with_constants(code, [b'0', b'gameMode.allowedBikes.5', b'gameMode.allowedBikes.6'])
    assert idx == [zero, five, six]
    return code


# ---- SWF ----
def tags(body, pos):
    while pos < len(body):
        rec = struct.unpack_from('<H', body, pos)[0]
        code, n, hdr = rec >> 6, rec & 0x3F, 2
        if n == 0x3F:
            n, hdr = struct.unpack_from('<I', body, pos + 2)[0], 6
        yield pos, code, hdr, n
        pos += hdr + n
        if code == 0:
            return


def patch_swf(swf):
    if swf[:3] == b'CWS':
        body = swf[:8] + zlib.decompress(swf[8:])
    elif swf[:3] == b'FWS':
        body = swf
    else:
        raise ValueError('not a SWF')
    nbits = body[8] >> 3
    first = 8 + (5 + 4 * nbits + 7) // 8 + 4
    for pos, code, hdr, n in tags(body, first):
        if code != 59:                                    # DoInitAction
            continue
        data = body[pos + hdr:pos + hdr + n]
        if b'EditorTrackSettingsMenuPC' not in data or LAST not in data:
            continue
        new = patch_class(data[2:])
        if new is None:
            return None
        new = data[:2] + new
        rec = struct.pack('<HI', (59 << 6) | 0x3F, len(new))
        body = body[:pos] + rec + new + body[pos + hdr + n:]
        body = body[:4] + struct.pack('<I', len(body)) + body[8:]
        return body[:8] + zlib.compress(body[8:], 9) if swf[:3] == b'CWS' else body
    raise ValueError('track settings class not found')


def main(game):
    path = os.path.join(game, 'build', 'data', 'data_misc.pak')
    d, entries = read_pak(path)
    named = names_of(d, entries)
    changed = {}
    for name, f in named:
        if name in MENUS:
            try:
                new = patch_swf(file_data(d, f))
            except ValueError as e:
                print(f'{name}: not patched ({e})')
                continue
            if new:
                changed[name] = new
    if not changed:
        print('editor track settings: DLC bikes already allowed')
        return []
    out, names = [], []
    for name, f in named:
        if name in changed:
            out.append((f['hash'], 8, zlib.compress(changed[name], 9), len(changed[name])))
        else:
            out.append((f['hash'], f['method'], d[f['offset']:f['offset'] + f['packed']], f['size']))
        names.append(name)
    if any(f['hash'] == NAME_TABLE for f in entries):
        blob = struct.pack('<I', len(names)) + b''.join(struct.pack('<H', len(n)) + n.encode('latin-1') for n in names)
        out.append((NAME_TABLE, 0, blob, len(blob)))
    tmp = path + '.editor-bikes.tmp'
    write_pak(tmp, out)
    d2, e2 = read_pak(tmp)
    check = dict(names_of(d2, e2))
    for name, new in changed.items():
        assert file_data(d2, check[name]) == new
    if not os.path.exists(path + BACKUP):
        shutil.copy2(path, path + BACKUP)
    os.replace(tmp, path)
    print('editor track settings: Gecko 520 and Banshee can be allowed (' + ', '.join(changed) + ')')
    return list(changed)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
