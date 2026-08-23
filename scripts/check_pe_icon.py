from __future__ import annotations

import struct
import sys
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14
IDI_SNAPLITE = 101


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
    count = named + ids
    entry_off = off + 16
    for i in range(count):
        current = entry_off + i * 8
        name = u32(data, current)
        target = u32(data, current + 4)
        yield name, target


def find_id_entry(data: bytes, resource_base: int, directory_rel: int, wanted_id: int):
    for name, target in directory_entries(data, resource_base, directory_rel):
        if name & 0x80000000:
            continue
        if (name & 0xFFFF) == wanted_id:
            return target
    return None


def has_resource_id(data: bytes, resource_base: int, resource_type: int, resource_id: int) -> bool:
    type_target = find_id_entry(data, resource_base, 0, resource_type)
    if type_target is None or not (type_target & 0x80000000):
        return False
    type_dir_rel = type_target & 0x7FFFFFFF
    id_target = find_id_entry(data, resource_base, type_dir_rel, resource_id)
    return id_target is not None


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

    if not has_resource_id(data, resource_base, RT_GROUP_ICON, IDI_SNAPLITE):
        raise ValueError('Missing RT_GROUP_ICON resource ID 101')
    if not has_resource_id(data, resource_base, RT_ICON, 1):
        raise ValueError('Missing RT_ICON image resource')

    print(f'Embedded Snap-Lite icon verified in {exe}')


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
