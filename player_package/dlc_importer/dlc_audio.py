"""Convert Trials Evolution Xbox FSB4/XMA2 banks to verified PC PCM16 FSB4.

Local use only. No audio is uploaded. Python 3.10+ and vgmstream-cli required.
PCM16 is the AUDIO_BRIEF.md fallback; this is NOT an IMA ADPCM encoder.
Each output is published only after all subsounds round-trip byte-exactly.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


def require(condition, message):
    if not condition:
        raise ValueError(message)


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_bank(path):
    data = path.read_bytes()
    require(data[:4] == b'FSB4', f'{path}: not FSB4')
    count, header_size, data_size, version, flags = struct.unpack_from('<5I', data, 4)
    require(version == 0x40000 and flags == 0x40, 'Unsupported FSB4 version/flags')
    require(48 + header_size + data_size == len(data), 'Incorrect bank size')
    data_offset = 48 + header_size
    require(data_offset % 32 == 0, 'Unaligned data section')
    offset = 48
    samples = []
    for index in range(count):
        size = struct.unpack_from('<H', data, offset)[0]
        require(size >= 80 and offset + size <= 48 + header_size, 'Bad sample header')
        header = data[offset:offset + size]
        frames, stored, start, end, mode, rate = struct.unpack_from('<6I', header, 32)
        channels = struct.unpack_from('<H', header, 62)[0]
        require(channels in (1, 2) and rate > 0 and frames > 0, 'Unsupported audio parameters')
        require(not mode & 0x8000, 'Duplicate sample storage is not implemented')
        require(data_offset + stored <= len(data), 'Sample outside bank')
        require(start <= end < frames, 'Invalid loop bounds')
        samples.append(dict(index=index + 1, header=header, frames=frames, stored=stored,
                            start=start, end=end, mode=mode, rate=rate, channels=channels,
                            name=header[2:32].split(b'\0', 1)[0].decode('ascii'),
                            data_offset=data_offset))
        offset += size
        data_offset = (data_offset + stored + 31) & ~31
    require(not any(data[offset:48 + header_size]), 'Unexpected header padding')
    require(data_offset == len(data), 'Unexpected trailing data')
    return data, samples


def sync_data(sample):
    """Remove XMA extension (16 bytes + seek table), retaining exact SYNC bytes."""
    header = sample['header']
    require(len(header) >= 96, 'Missing XMA extension')
    end = 96 + u32(header, 92)
    require(end <= len(header), 'Invalid XMA seek table size')
    sync = header[end:]
    if sample['mode'] & 0x80000000:
        require(sync[:4] == b'SYNC' and len(sync) >= 8, 'Missing SYNC extension')
        count = u32(sync, 4)
        stride = 4 if sample['mode'] & 0x4000 else 260
        require(len(sync) == 8 + count * stride, 'Unsupported SYNC layout')
        for index in range(count):
            require(u32(sync, 8 + index * stride) < sample['frames'], 'SYNC outside sample')
    else:
        require(not sync, 'Unknown XMA extension suffix')
    return sync


def pcm_header(sample, byte_count):
    header = bytearray(sample['header'][:80])
    header.extend(sync_data(sample))
    # These banks mirror the header size in the legacy varfreq/size_32bits field.
    require(u32(header, 72) == len(sample['header']), 'Unexpected size_32bits field')
    struct.pack_into('<H', header, 0, len(header))
    struct.pack_into('<I', header, 72, len(header))
    struct.pack_into('<I', header, 36, byte_count)
    # Preserve loop/playback/SYNC flags, remove XMA, declare signed interleaved PCM16.
    require(sample['mode'] & 0x01000000, 'Source sample is not XMA')
    require(sample['mode'] & ~0x81004007 == 0, 'Unexpected source mode')
    mode = (sample['mode'] & ~0x01000000) | 0x10 | 0x100
    mode |= 0x20 if sample['channels'] == 1 else 0x40
    struct.pack_into('<I', header, 48, mode)
    return bytes(header)


def decode(cli, bank, index, output):
    result = subprocess.run([str(cli), '-i', '-I', '-s', str(index), '-o', str(output), str(bank)],
                            capture_output=True, text=True, encoding='utf-8', errors='replace')
    require(result.returncode == 0, f'Decode failed {bank} #{index}: {result.stderr}')
    # Decoder warnings are treated as errors instead of silently accepting damaged input.
    require(not result.stderr.strip(), f'Decoder warning {bank} #{index}: {result.stderr}')
    metadata = json.loads(result.stdout)
    with wave.open(str(output), 'rb') as wav:
        require(wav.getsampwidth() == 2 and wav.getcomptype() == 'NONE', 'Expected PCM16 WAV')
        params = (wav.getframerate(), wav.getnchannels(), wav.getnframes())
        pcm = wav.readframes(wav.getnframes())
    require(len(pcm) == params[1] * params[2] * 2, 'Truncated decoded WAV')
    return pcm, params, metadata


def convert_bank(source, destination, cli, work_root):
    original, samples = read_bank(source)
    require(not destination.exists(), f'Refusing to overwrite {destination}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    headers = [pcm_header(s, s['frames'] * s['channels'] * 2) for s in samples]
    header_data = b''.join(headers)
    header_data += bytes(-(48 + len(header_data)) % 32)
    data_size = sum((s['frames'] * s['channels'] * 2 + 31) & ~31 for s in samples)
    main = bytearray(original[:48])
    struct.pack_into('<II', main, 8, len(header_data), data_size)
    # Keep bank identity, names, indices, rates, channel counts, loop and sync coordinates.
    # vgmstream removes XMA codec delay internally; sample coordinates do not resample.
    report = dict(source=str(source), output=str(destination), codec='PCM16LE',
                  source_sha256=digest(original), samples=[], verified=False)
    with tempfile.TemporaryDirectory(prefix='dlc_audio_', dir=work_root) as temp:
        temp = Path(temp)
        staged = temp / destination.name
        wav_path = temp / 'sample.wav'
        with staged.open('wb') as output:
            output.write(main)
            output.write(header_data)
            for sample in samples:
                pcm, params, metadata = decode(cli, source, sample['index'], wav_path)
                require(params == (sample['rate'], sample['channels'], sample['frames']),
                        f"Source length/rate mismatch: {source} #{sample['index']}: {params}")
                output.write(pcm)
                output.write(bytes(-len(pcm) % 32))
                report['samples'].append(dict(index=sample['index'], name=sample['name'],
                    frames=sample['frames'], rate=sample['rate'], channels=sample['channels'],
                    loop_start=sample['start'], loop_end_inclusive=sample['end'],
                    sync_sha256=digest(sync_data(sample)), pcm_sha256=digest(pcm),
                    decoder_metadata=metadata))
                if sample['index'] % 25 == 0:
                    print(f"  decoded {sample['index']}/{len(samples)}", flush=True)
        converted, new_samples = read_bank(staged)
        require(len(new_samples) == len(samples), 'Sample count changed')
        for old, new, record, expected_header in zip(samples, new_samples, report['samples'], headers):
            require(new['header'] == expected_header, 'Written header mismatch')
            require(new['header'][2:32] == old['header'][2:32], 'Sample name changed')
            require(new['header'][40:48] == old['header'][40:48], 'Loop coordinates changed')
            require(new['header'][80:] == sync_data(old), 'SYNC markers changed')
            pcm, params, metadata = decode(cli, staged, new['index'], wav_path)
            require(params == (old['rate'], old['channels'], old['frames']), 'Output parameters changed')
            require(digest(pcm) == record['pcm_sha256'], f"PCM mismatch at sample {new['index']}")
            require(metadata['numberOfSamples'] == record['decoder_metadata']['numberOfSamples'],
                    'Decoded frame count changed')
            require(metadata['loopingInfo'] == record['decoder_metadata']['loopingInfo'],
                    'Decoded loop metadata changed')
            record['verified_pcm_sha256'] = digest(pcm)
            if new['index'] % 25 == 0:
                print(f"  verified {new['index']}/{len(samples)}", flush=True)
        report.update(verified=True, output_sha256=digest(converted), output_bytes=len(converted),
                      sample_count=len(samples))
        # Exclusive creation protects existing outputs. Only fully validated banks reach here.
        with destination.open('xb') as output:
            output.write(converted)
    report_path = destination.with_suffix('.verification.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS {destination.name}: {len(samples)} samples; {len(converted):,} bytes', flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path('O:/trialsevo/dlc_unpacked'))
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--vgmstream', type=Path,
                        default=Path(__file__).parent / 'audio/vgmstream/vgmstream-cli.exe')
    parser.add_argument('--bank', help='Optional exact filename for a single-bank trial')
    parser.add_argument('--work-dir', type=Path, default=Path(__file__).parent / 'audio/work')
    args = parser.parse_args()
    args.vgmstream = args.vgmstream.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    banks = sorted(args.source_root.glob('*/soundsystem/xbox/*.fsb'))
    if args.bank:
        banks = [bank for bank in banks if bank.name == args.bank]
    require(banks, 'No input banks found')
    reports = []
    for bank in banks:
        target = args.output_root / bank.parents[2].name / 'soundsystem/win' / bank.name
        print(f'Converting {bank}', flush=True)
        reports.append(convert_bank(bank, target, args.vgmstream, args.work_dir))
    summary = dict(codec='PCM16LE', verified=True, banks=len(reports),
                   samples=sum(r['sample_count'] for r in reports),
                   bytes=sum(r['output_bytes'] for r in reports),
                   outputs=[{k: r[k] for k in ('output', 'sample_count', 'output_bytes',
                                              'source_sha256', 'output_sha256')} for r in reports])
    args.output_root.mkdir(parents=True, exist_ok=True)
    summary_path = args.output_root / ('audio_verification_' + (args.bank or 'all') + '.json')
    summary_path.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'outputs'}), flush=True)


if __name__ == '__main__':
    main()
