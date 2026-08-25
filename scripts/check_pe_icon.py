from __future__ import annotations

import struct
import sys
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14
IDI_SNAPLITE = 101
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
REQUIRED_SIZES = {(16, 16), (32, 32), (48, 48)}


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from('<H', data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from('<I', data, off)[0]


def rva_to_offset(data: bytes, section_off: int, section_count: int, rva: int) -> int:
    for i in range(section_count):
        off = section_off + i * 40
        virtual_size = u32(data, off + 8)
        virtual_address = u32(data, off + 12)
        raw_size = u32(data, off + 16)
        raw_ptr = u32(data, off + 20)
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return raw_ptr + (rva - virtual_address)
    raise ValueError(f'RVA 0x{rva:x} is not inside any PE section')


def directory_entries(data: bytes, resource_base: int, directory_rel: int):
    off = resource_base + directory_rel
    named = u16(data, off + 12)
    ids = u16(data, off + 14)
    entry_off = off + 16
    for i in range(named + ids):
        current = entry_off + i * 8
        yield u32(data, current), u32(data, current + 4)


def find_id_entry(data: bytes, resource_base: int, directory_rel: int, wanted_id: int):
    for name, target in directory_entries(data, resource_base, directory_rel):
        if name & 0x80000000:
            continue
        if (name & 0xFFFF) == wanted_id:
            return target
    return None


def resource_payload(
    data: bytes,
    resource_base: int,
    section_off: int,
    section_count: int,
    resource_type: int,
    resource_id: int,
) -> bytes:
    type_target = find_id_entry(data, resource_base, 0, resource_type)
    if type_target is None or not (type_target & 0x80000000):
        raise ValueError(f'Missing resource type {resource_type}')

    type_dir = type_target & 0x7FFFFFFF
    id_target = find_id_entry(data, resource_base, type_dir, resource_id)
    if id_target is None or not (id_target & 0x80000000):
        raise ValueError(f'Missing resource type {resource_type} id {resource_id}')

    language_dir = id_target & 0x7FFFFFFF
    language_entries = list(directory_entries(data, resource_base, language_dir))
    if not language_entries:
        raise ValueError(f'Resource type {resource_type} id {resource_id} has no language entry')

    data_target = language_entries[0][1]
    if data_target & 0x80000000:
        raise ValueError('Unexpected extra resource directory level')

    data_entry = resource_base + data_target
    payload_rva = u32(data, data_entry)
    payload_size = u32(data, data_entry + 4)
    payload_off = rva_to_offset(data, section_off, section_count, payload_rva)
    payload = data[payload_off:payload_off + payload_size]
    if len(payload) != payload_size:
        raise ValueError('Resource payload is truncated')
    return payload


def png_dimensions(payload: bytes) -> tuple[int, int]:
    if len(payload) < 24 or not payload.startswith(PNG_SIGNATURE):
        raise ValueError('Icon image is not PNG-compressed')
    return struct.unpack_from('>II', payload, 16)


def check_icon(exe: Path) -> None:
    data = exe.read_bytes()
    if len(data) < 0x100 or data[:2] != b'MZ':
        raise ValueError('Not a valid PE executable: missing MZ header')

    pe_off = u32(data, 0x3C)
    if data[pe_off:pe_off + 4] != b'PE\0\0':
        raise ValueError('Not a valid PE executable: missing PE signature')

    coff = pe_off + 4
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    magic = u16(data, optional)
    if magic == 0x20B:
        data_directory = optional + 112
    elif magic == 0x10B:
        data_directory = optional + 96
    else:
        raise ValueError(f'Unsupported PE optional-header magic: 0x{magic:x}')

    resource_rva = u32(data, data_directory + 2 * 8)
    resource_size = u32(data, data_directory + 2 * 8 + 4)
    if resource_rva == 0 or resource_size == 0:
        raise ValueError('PE file has no resource directory')

    section_off = optional + optional_size
    resource_base = rva_to_offset(data, section_off, section_count, resource_rva)

    group = resource_payload(
        data,
        resource_base,
        section_off,
        section_count,
        RT_GROUP_ICON,
        IDI_SNAPLITE,
    )
    if len(group) < 6:
        raise ValueError('RT_GROUP_ICON payload is truncated')

    reserved, icon_type, count = struct.unpack_from('<HHH', group, 0)
    if reserved != 0 or icon_type != 1 or count < 1:
        raise ValueError('RT_GROUP_ICON header is invalid')
    if len(group) < 6 + count * 14:
        raise ValueError('RT_GROUP_ICON directory is truncated')

    found_sizes: set[tuple[int, int]] = set()
    details: list[str] = []

    for i in range(count):
        off = 6 + i * 14
        width_byte, height_byte, _colors, _reserved, planes, bit_count, bytes_in_res, image_id = (
            struct.unpack_from('<BBBBHHIH', group, off)
        )
        width = 256 if width_byte == 0 else width_byte
        height = 256 if height_byte == 0 else height_byte

        payload = resource_payload(
            data,
            resource_base,
            section_off,
            section_count,
            RT_ICON,
            image_id,
        )
        if len(payload) != bytes_in_res:
            raise ValueError(
                f'RT_ICON {image_id} size mismatch: group={bytes_in_res}, resource={len(payload)}'
            )

        actual_width, actual_height = png_dimensions(payload)
        if (actual_width, actual_height) != (width, height):
            raise ValueError(
                f'RT_ICON {image_id} metadata {width}x{height} does not match '
                f'PNG payload {actual_width}x{actual_height}'
            )
        if planes != 1 or bit_count not in (0, 32):
            raise ValueError(
                f'RT_ICON {image_id} has unexpected planes/bit depth: {planes}/{bit_count}'
            )

        found_sizes.add((width, height))
        details.append(f'{width}x{height}#id{image_id}')

    missing = REQUIRED_SIZES - found_sizes
    if missing:
        formatted = ', '.join(f'{w}x{h}' for w, h in sorted(missing))
        raise ValueError(f'Missing required embedded icon sizes: {formatted}')

    print(f'Embedded Snap-Lite icon verified in {exe}: {", ".join(details)}')


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_pe_icon.py <SnapLite.exe>', file=sys.stderr)
        return 2
    try:
        check_icon(Path(sys.argv[1]))
        return 0
    except Exception as exc:
        print(f'icon verification failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
