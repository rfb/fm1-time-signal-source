#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Linux port of sdk/Build.ps1 -FirmwareOnly, using JieLi's Linux toolchain.

Keeps every Build.ps1 check: SDK payload and DSP contracts, ELF symbol,
SRAM placement, link map and section rules. Only the toolchain identity
differs: the 16 compiler headers are verified against sdk/manifest.json
(after Windows line-ending normalisation) and the Linux programs against
tools/linux-toolchain.json. Packaging ports sdk/Package.cs (MIT).

    python3 -I tools/build_linux.py --toolchain <jieli-linux-toolchains-dir>
"""
import argparse, hashlib, json, os, re, resource, struct, subprocess, sys
from pathlib import Path

PROFILE = 'dx7'
ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / 'sdk'
BUILD = ROOT / 'build' / PROFILE
VARIANT = {'user_bytes': 536576, 'frozen': 'sdk/lib/libfm1_engine.a', 'update_family': 7}
# Windows toolchain files and their Linux counterparts in the JieLi tarball.
PROGRAMS = {'clang': 'common/bin/clang', 'ld': 'pi32v2/bin/ld', 'gold': 'common/bin/LLVMgold.so',
            'nm': 'pi32v2/bin/nm', 'objcopy': 'common/bin/objcopy'}


class BuildError(Exception):
    pass


def fail(message):
    raise BuildError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_text(path, text):
    Path(path).write_bytes(text.encode('utf-8'))


def write_json(path, value):
    write_text(path, json.dumps(value, indent=2))


def run(program, arguments, cwd):
    result = subprocess.run([str(program)] + [str(a) for a in arguments], cwd=cwd)
    if result.returncode:
        fail(f'{Path(program).name} failed ({result.returncode})')


def windows_sort_key(relative):
    # Build.ps1 sorts FullName case-insensitively with '\' separators.
    return relative.replace('/', '\\').lower()


def visible_files(directory):
    # Get-ChildItem without -Force skips hidden (dot) files.
    for path in sorted(directory.rglob('*')):
        if path.is_file() and not any(p.startswith('.') for p in path.relative_to(directory).parts):
            yield path


def verify_toolchain(toolchain, manifest):
    contract = json.loads((ROOT / 'tools' / 'linux-toolchain.json').read_text())
    if contract.get('schema') != 1:
        fail('Invalid Linux toolchain contract.')
    external = manifest.get('external_toolchain')
    if not external or external.get('schema') != 1 or not external.get('files'):
        fail('Invalid external toolchain manifest.')
    for required in ('bin/clang.exe', 'bin/pi32v2-ld.exe', 'bin/LLVMgold.dll', 'bin/llvm-nm.exe', 'bin/llvm-objcopy.exe'):
        if required not in external['files']:
            fail(f'Missing toolchain contract: {required}')
    for name, expected in external['files'].items():
        if not re.fullmatch(r'(bin|lib)/[A-Za-z0-9_./-]+', name) or '..' in name.split('/') or not re.fullmatch(r'[a-f0-9]{64}', expected):
            fail('Invalid external toolchain entry.')
        if name.startswith('bin/'):
            if name not in contract['windows_equivalents']:
                fail(f'No Linux equivalent recorded for {name}')
            continue
        # Shared headers: identical to Windows 2.5.3 once LF becomes CRLF.
        path = toolchain / 'common' / name
        if not path.is_file():
            fail(f'External toolchain does not match the SDK: {name}')
        data = path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        if hashlib.sha256(data).hexdigest() != expected:
            fail(f'External toolchain does not match the SDK: {name}')
    for name, expected in contract['files'].items():
        path = toolchain / name
        if not path.is_file() or sha256(path) != expected:
            fail(f'Linux toolchain does not match tools/linux-toolchain.json: {name}. '
                 f'Expected {contract["release"]}.')
    return {key: toolchain / rel for key, rel in PROGRAMS.items()}


def test_firmware(tools, extra, synth):
    elf = BUILD / 'sdk.elf'
    report = BUILD / 'firmware-report.json'
    write_text(report, '{"passed":false}')
    result = subprocess.run([str(tools['nm']), '--print-size', str(elf)], capture_output=True, text=True)
    if result.returncode:
        fail('Cannot inspect ELF symbols.')
    info, heap, usb_midi = {}, {}, False
    for line in result.stdout.splitlines():
        m = re.match(r'^([0-9a-fA-F]+)[ \t]+(([0-9a-fA-F]+)[ \t]+)?([A-Za-z])[ \t]+([^\r]+)', line)
        if not m:
            continue
        name, kind = m.group(5), m.group(4)
        info[name] = {'address': int(m.group(1), 16), 'size': int(m.group(3), 16) if m.group(3) else None, 'kind': kind}
        if re.match(r'^(__fat_|fat_sdfile_fat_ops$|jl_fat_vfs_ops$|nor_sdfile_vfs_ops$|sdfile_ext_vfs_ops$)', name):
            fail(f'Unused filesystem entered the fixed Flash runtime: {name}')
        if 'HEAP' in name or ('heap' in name and kind == 'A'):
            heap[name] = info[name]['address']
        if name.startswith('lv_'):
            fail(f'Unexpected LVGL runtime: {name}')
        if re.search(r'usb.*midi|midi.*usb', name):
            usb_midi = True
    registrations = [('initcall', 4, 4), ('early_initcall', 16, 4), ('late_initcall', 4, 4),
                     ('platform_initcall', 4, 4), ('module_initcall', 4, 4), ('sys_event_handler', 12, 12),
                     ('syscfg_ops', 84, 28), ('lp_target', 72, 8), ('sys_cpu_timer', 16, 16),
                     ('clock_critical_handler', 16, 8), ('vfs_ops', 120, 120),
                     ('audio_decoder', 56, 56), ('audio_hwaccel', 32, 32)]
    for name, minimum, stride in registrations:
        begin, end = f'_{name}_begin', f'_{name}_end'
        if begin not in info or end not in info:
            begin, end = f'{name}_begin', f'{name}_end'
        if begin not in info or end not in info:
            fail(f'Runtime registration range missing: {name}')
        size = info[end]['address'] - info[begin]['address']
        if size < minimum or size % stride:
            fail(f'Runtime registration table invalid: {name}')
        if name == 'vfs_ops' and size != 120:
            fail('Expected only the internal Flash filesystem registration.')
    required = ['fm1_r_7919135c9963', 'fm1_r_1f449c2c7b8f', 'vSoftwareInterruptISR', 'fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b',
                'fm1_update_init', 'fm1_update_message', 'fm1_update_busy', 'fm1_build_id', 'fm1_flash_init',
                'sdfile_vfs_ops', 'sdfile_init', 'syscfg_vm_init', 'syscfg_btif_init',
                'wifi_get_xosc', 'wifi_get_mcs_dgain', 'wifi_get_pa_trim_data',
                'connectivity_wl82_bluetooth_start', 'ble_profile_init', 'att_server_init',
                'fm1_ble_set_role', 'fm1_ble_scan', 'fm1_ble_connect', 'fm1_ble_disconnect', 'fm1_ble_get_status', 'fm1_ble_get_device',
                'audio_app_usb_microphone_enable', 'audio_app_usb_speaker_enable',
                'connectivity_audio_usb_microphone_enable', 'connectivity_audio_usb_speaker_enable',
                'audio_app_bluetooth_open', 'sbc_decoder', 'sbc_hwaccel', 'avdtp_packet_handler', 'bt_get_local_name', 'bt_get_mac_addr',
                'fm1_bluetooth_configure', 'fm1_bluetooth_get_status',
                'groove_tuning_capture', 'groove_tuning_apply', 'groove_project_apply_edit']
    for name in required:
        if name not in info:
            fail(f'Required runtime symbol absent: {name}')
    for name in ('sdp_send_cmd_iotl', 'sdp_master_packet_handler', 'gatt_client_discover_primary_services_by_uuid128'):
        if name not in info or info[name]['kind'] not in 'TtNn' or (info[name]['size'] or 0) <= 16:
            fail(f'Bluetooth discovery implementation missing or stubbed: {name}')
    for name in ('fm1_r_dee1ea411566', 'fm1_r_d16405f329ed', 'fm1_r_f03cc16d330d', 'fm1_r_1993f9b103e3',
                 'fm1_io_trs_midi_enabled', 'fm1_r_05431712fb7b', 'fm1_r_0aa010fe1de1', 'fm1_r_33d5e5b63b6b', 'fm1_app_sysex'):
        if name not in info or info[name]['kind'] not in 'TNtn' or not info[name]['size']:
            fail(f'Application callback was lost or replaced by a weak default: {name}')
    if info.get('fm1_build_id', {}).get('size') != 32:
        fail('Browser update build identity must be exactly 32 bytes.')
    if info.get('fm1_update_identity', {}).get('size') != 27:
        fail('FM1 update identity must be exactly 27 bytes.')
    if not usb_midi:
        fail('USB MIDI runtime is missing.')
    for name in ('CurrentTCB', 'highCurrentTCB', 'xIdleTaskHandle'):
        if name not in info or info[name]['size'] != 4:
            fail(f'Platform RTOS ABI is invalid: {name}')
    for name in ('__rt_local_irq_disable', '__rt_local_irq_enable', 'bt_lock_cnt',
                 'fm1_r_83b2de623a96', 'fm1_r_c327ea18da09', 'cpuSuspend', 'audio_app_worker', 'edr_name', 'ble_name',
                 'a2dp_source_event_handler', 'a2dp_source_sbc_codec', 'hfp_profile_init', 'hfp_ag_profile_init',
                 'spp_user_send_data', 'avrcp_player_event', 'hid_diy_regiest_callback', 'obex_connection_req',
                 'pan_cmd_io_ctrl', 'gatt_client_discover_primary_services', 'ancs_client_init', 'ams_client_init',
                 'sdio_host_init', 'wl30_sdio_claim_irq', 'wl30_sdio_release_irq', 'wifi_on', 'wifi_off',
                 'wifi_rx_task', 'wifi_tx_task', 'ui_yuv420_to_rgb565_pairs', 'ui_tiny_ttf_pack_scanline'):
        if name in info:
            fail(f'Unsupported platform runtime entered FM1: {name}')
    link_map = (BUILD / 'sdk.map').read_text(encoding='utf-8', errors='replace')
    if VARIANT['frozen'] + '(' not in link_map:
        fail('Fixed native SDK archive missing.')
    if 'libfm1_runtime.a(' in link_map:
        fail('Only the fixed music/BLE SDK may be linked.')
    for item in ('/system.a(', 'audio_server.a(', 'media_app.a(', 'btstack.a(', 'lib_ui.a(', 'libjpeg_decode_only.a('):
        if item in link_map:
            fail(f'Unsupported FM1 link input: {item}')
    for member in ('a2dp_source', 'ams_client', 'ancs_client', 'avctp', 'avrcp', 'bip', 'bnep', 'bt_audio',
                   'client_user', 'hfp_ag_profile', 'hfp_profile', 'hid', 'iap_profile', 'obex', 'opp', 'pan',
                   'pbg_profile', 'rfcomm', 'spp_profile', 'spp_update_profile', 'spp_user', 'vCard'):
        if f'.a({member}.c.o)' in link_map:
            fail(f'Unsupported FM1 link member: {member}')
    acl = {}
    for direction in ('tx', 'rx'):
        begin = f'acl_{direction}_pool'
        end = begin + '_end'
        if begin not in info or end not in info:
            fail(f'Required controller pool missing: {begin}')
        acl[direction] = info[end]['address'] - info[begin]['address']
    if acl['tx'] < 1024 or acl['rx'] < 4096:
        fail('Classic ACL pools too small; need 1024/4096 bytes.')
    m = re.search(r'0x([0-9a-fA-F]+)[^\r\n]*_MALLOC_SIZE[ ]*=', link_map)
    if not m:
        fail('Cannot find runtime heap in link map.')
    heap_bytes = int(m.group(1), 16)
    if heap_bytes < 49152:
        fail(f'FM1 runtime heap is {heap_bytes} bytes; need at least 49152.')
    callbacks = ['fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b', 'fm1_r_ffe3f82683f6', 'fm1_audio_irq', 'fm1_flash_worker_prepare',
                 'fm1_r_7e12ed19990d', 'fm1_r_74310ca1fed9',
                 'fm1_engine_render', 'fm1_engine_output_level', 'fm1_audio_copy', 'fm1_output_mix',
                 'fm1_usb_audio_read', 'fm1_bluetooth_audio_read', 'fm1_usb_audio_write', 'connectivity_audio_get_speaker_volume',
                 'connectivity_audio_get_mic_volume'] + list(extra)
    callbacks += [n for n in list(synth.get('sram_optional') or []) + list(synth.get('native_sram_optional') or []) if n in info]
    callbacks += ['cpu_enter_sfc_critical', 'cpu_exit_sfc_critical', 'enter_spi_code', 'exit_spi_code', 'fm1_r_da3931a5b7a8', 'sfc_drop_cache']
    callbacks += [n for n in ('audio_app_usb_mix', 'audio_app_usb_record', 'audio_app_usb_replace_input',
                              'render_stereo', 'effects_render', 'effects_sample', 'sequence_event', 'clock_publish',
                              'groove_arp_event', 'groove_arp_running', 'engine_note') if n in info]
    engine_ready = 'fm1_r_0aa010fe1de1' in info and info['fm1_r_0aa010fe1de1']['address'] > 0
    if engine_ready:
        callbacks.append('fm1_r_0aa010fe1de1')
        if 'fm1_r_33d5e5b63b6b' in info and info['fm1_r_33d5e5b63b6b']['address'] > 0:
            callbacks.append('fm1_r_33d5e5b63b6b')
    for name in callbacks:
        if name not in info:
            fail(f'Audio callback absent: {name}')
        if not 0x01c00000 <= info[name]['address'] < 0x01c80000:
            fail(f'Audio callback is outside internal SRAM: {name}')
    for name in ('fm1_input_tick', 'fm1_panel_irq', 'fm1_scan_decode', 'fm1_key_pin', 'fm1_encoder_pin', 'fm1_quadrature'):
        if name not in info or not 0x01c00000 <= info[name]['address'] < 0x01c80000:
            fail(f'Panel scan code or wiring table is outside internal SRAM: {name}')

    data = elf.read_bytes()

    def check_range(offset, count):
        if offset < 0 or count < 0 or offset > len(data) or count > len(data) - offset:
            fail('Truncated ELF.')

    def u16(offset):
        check_range(offset, 2)
        return struct.unpack_from('<H', data, offset)[0]

    def u32(offset):
        check_range(offset, 4)
        return struct.unpack_from('<I', data, offset)[0]

    check_range(0, 52)
    if data[:6] != b'\x7fELF\x01\x01':
        fail('Expected ELF32 little endian.')
    entry = u32(24)
    if (entry != 0x02000120 or info.get('_start', {}).get('address') != entry or
            info.get('_text_rodata_begin', {}).get('address') != entry):
        fail('Firmware startup must match the fixed ROM entry and the first packaged instruction.')
    table, stride, count, strings_index = u32(32), u16(46), u16(48), u16(50)
    if stride < 40 or not count or strings_index >= count:
        fail('Invalid ELF section table.')
    check_range(table, stride * count)
    strings_header = table + strings_index * stride
    strings_offset, strings_bytes = u32(strings_header + 16), u32(strings_header + 20)
    check_range(strings_offset, strings_bytes)
    sections, payload, app_image = [], 0, 0
    for i in range(count):
        header = table + i * stride
        name_offset, kind, flags = u32(header), u32(header + 4), u32(header + 8)
        address, offset, size = u32(header + 12), u32(header + 16), u32(header + 20)
        if kind != 8:
            check_range(offset, size)
        if not flags & 2 or not size:
            continue
        if name_offset >= strings_bytes:
            fail('ELF section name is outside string table.')
        start = end = strings_offset + name_offset
        while end < strings_offset + strings_bytes and data[end]:
            end += 1
        if end >= strings_offset + strings_bytes:
            fail('Unterminated ELF section name.')
        name = data[start:end].decode('ascii')
        if not re.fullmatch(r'\.(text|data|bss|ram0_data|ram0_bss|boot_info|cache_ram_data|cache_ram_bss)', name):
            fail(f'Unsupported allocated section: {name}')
        nobits = kind == 8
        if not nobits:
            payload += size
            if re.fullmatch(r'\.(text|data|ram0_data|cache_ram_data)', name):
                app_image += size
            elif name != '.boot_info':
                fail(f'Allocated section is not covered by FM1 packaging: {name}')
        sections.append({'name': name, 'address': address, 'bytes': size, 'nobits': nobits})
    write_json(report, {
        'passed': True, 'elf_sha256': sha256(elf), 'allocated_image_bytes': payload, 'app_image_bytes': app_image,
        'startup_entry_verified': True, 'startup_entry': entry, 'instrument_ready': engine_ready,
        'image_definition': 'Sum of ELF allocated non-NOBITS sections; excludes bootloader/filesystem packaging',
        'sections': sections, 'heap_symbols': heap, 'runtime_heap_bytes': heap_bytes, 'minimum_runtime_heap_bytes': 49152,
        'classic_acl_tx_bytes': acl['tx'], 'classic_acl_rx_bytes': acl['rx'], 'registration_tables_verified': True,
        'rtos_abi_verified': True, 'audio_callbacks_in_sram': True, 'extra_audio_symbols_in_sram': list(extra),
        'bluetooth_and_usb_midi_linked': True, 'usb_audio_playback_and_capture_linked': True, 'bluetooth_audio_absent': False,
        'ble_roles': 'peripheral/central', 'lvgl_absent': True, 'active_bluetooth_discovery_linked': True,
        'application_callbacks_verified': True, 'board_validation': 'not performed', 'toolchain': 'jieli-linux'})
    print(f'FM1: {app_image} bytes app image; SRAM/BLE/USB checks passed')


# --- sdk/Package.cs port --------------------------------------------------
AREA, ENTRY_POINT, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MINIMUM_VM = 0xff000, 536576, 4096, 32768
PUBLIC_CODE = 0x980f


def _crc_table():
    table = []
    for i in range(256):
        value = i << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xffffffff
        table.append(value & 0xffff)
    return table


CRC_TABLE = _crc_table()


def crc(data, begin, length):
    value = 0
    for b in data[begin:begin + length]:
        value = ((value << 8) ^ CRC_TABLE[(value >> 8) ^ b]) & 0xffff
    return value


def cipher(data, begin, length, state):
    for i in range(begin, begin + length):
        data[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff


def round_up(size, alignment):
    return (size + alignment - 1) & -alignment


class Image:
    def __init__(self, size, fill):
        self.bytes = bytearray([fill]) * size

    def number(self, offset, value, width):
        self.bytes[offset:offset + width] = (value & ((1 << 8 * width) - 1)).to_bytes(width, 'little')

    def copy(self, offset, data):
        self.bytes[offset:offset + len(data)] = data

    def text(self, offset, value):
        self.copy(offset, value.encode('ascii'))

    def checksum(self, field, offset, size):
        self.number(field, crc(self.bytes, offset, size), 2)

    def record(self, offset, name, crc_value, address, size, attributes, last, extra):
        self.bytes[offset:offset + 32] = bytes(32)
        self.number(offset + 2, crc_value, 2)
        self.number(offset + 4, address, 4)
        self.number(offset + 8, size, 4)
        self.number(offset + 12, attributes, 2)
        self.number(offset + 14, last, 2)
        self.bytes[offset + 16:offset + 32] = b'\xff' * 16
        self.text(offset + 16, name)
        self.bytes[offset + 16 + len(name)] = 0
        if extra is not None:
            self.number(offset + 28, extra, 4)
        self.checksum(offset, offset + 2, 30)


def read_resource(directory, name, size):
    data = (directory / name).read_bytes()
    if len(data) != size:
        fail(f'Unexpected FM1 resource: {name}')
    return data


def encode_boot(plain):
    result = bytearray(plain)
    banks = struct.unpack_from('<H', plain, 0)[0]
    if banks < 1 or banks > len(plain) // 16:
        fail('Invalid boot directory.')
    previous_end = banks * 16
    for i in range(banks):
        header = i * 16
        length = struct.unpack_from('<H', plain, header + 2)[0]
        start = struct.unpack_from('<i', plain, header + 8)[0]
        if (length == 0 or start < previous_end or start > len(plain) - length or
                crc(plain, header, 14) != struct.unpack_from('<H', plain, header + 14)[0] or
                crc(plain, start, length) != struct.unpack_from('<H', plain, header + 12)[0]):
            fail('Invalid boot bank.')
        cipher(result, header, 16, 0xffff)
        cipher(result, start, length, 0xffff)
        previous_end = start + length
    return bytes(result)


def build_flash(application, directory, end, vm_end):
    boot = read_resource(directory, 'uboot.boot', 14488)
    config = read_resource(directory, 'boot-config.bin', 721)
    tool = read_resource(directory, 'cfg_tool.bin', 383)
    image, settings = Image(end, 0xff), Image(34 + len(config), 0)
    for bit in range(16):
        settings.bytes[bit] = 16
        settings.bytes[16 + bit] = 16 ^ (255 if ((PUBLIC_CODE >> bit) & 1) == 0 else 0)
    settings.checksum(32, 0, 32)
    settings.copy(34, config)
    image.copy(0, bytes([0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
                         0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]))
    image.checksum(0, 2, 30)
    config_at = 160 + len(boot)
    image.record(32, 'uboot.boot', crc(boot, 0, len(boot)), 160, len(boot), 0, 0, None)
    image.record(64, 'isd_config.ini', crc(settings.bytes, 0, len(settings.bytes)), config_at, len(settings.bytes), 0x8002, 0, None)
    image.record(96, 'app_dir_head', 0xffff, AREA, 0xffffffff, 0xff81, 0, None)
    image.record(128, 'key_mac', 0xffff, FLASH_LIMIT, 4096, 0x0112, 0xffff, None)
    for p in range(0, 160, 32):
        cipher(image.bytes, p, 32, 0xffff)
    image.copy(160, encode_boot(boot))
    image.copy(config_at, settings.bytes)
    payload_size = APP_DATA + len(application) + len(tool)
    app = AREA + APP_DATA
    image.copy(app, application)
    image.copy(app + len(application), tool)
    image.record(AREA + 32, 'app.bin', crc(application, 0, len(application)), APP_DATA, len(application), 0xff82, 0, None)
    image.record(AREA + 64, 'cfg_tool.bin', crc(tool, 0, len(tool)), APP_DATA + len(application), len(tool), 0xff82, 0, None)
    image.record(AREA + 96, 'VM', 0xffff, end, vm_end - end, 0x8112, 0, MINIMUM_VM)
    image.record(AREA + 128, 'PRCT', 0xffff, 0, end, 0x8292, 0, 0x45444f43)
    image.record(AREA + 160, 'BTIF', 0xffff, vm_end, BT_SIZE, 0x8192, 1, 0x4f545541)
    image.record(AREA, 'app_area_head', crc(image.bytes, AREA + 32, payload_size - 32), ENTRY_POINT, payload_size, 0xff83, 0, None)
    reserved = AREA + payload_size
    image.record(reserved + 32, 'USER', 0xffff, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4f545541)
    image.record(reserved, 'EXT_RESERVED', crc(image.bytes, reserved + 32, 32), 32, 64, 0xff93, 1, None)
    for p in range(0, payload_size + 64, 32):
        cipher(image.bytes, AREA + p, min(32, payload_size + 64 - p), PUBLIC_CODE ^ (p >> 2))
    return image


def wrap(flash, loader, product):
    if len(loader) < 1 or len(loader) >= 32768:
        fail('Invalid FM1 update loader.')
    loader_at = 1024 + round_up(len(flash.bytes), 32)
    total = loader_at + round_up(len(loader), 32)
    ufw = Image(total, 0xff)
    ufw.bytes[0:224] = bytes(224)
    files, names = (bytes(flash.bytes), loader), ('flash.bin', 'ota.bin')
    for i, data in enumerate(files):
        record = 64 + i * 80
        ufw.number(record, 0 if i == 0 else 100, 2)
        ufw.number(record + 2, i, 2)
        ufw.number(record + 4, crc(data, 0, len(data)), 2)
        ufw.number(record + 8, 1024 if i == 0 else loader_at, 4)
        ufw.number(record + 12, len(data), 4)
        ufw.number(record + 16, round_up(len(data), 32), 4)
        ufw.text(record + 64, names[i])
        cipher(ufw.bytes, record, 80, 0xffff)
    ufw.number(4, total, 4)
    ufw.number(8, 2, 2)
    ufw.number(10, 4, 2)
    ufw.number(12, 512, 4)
    ufw.text(16, 'AC791N')
    ufw.checksum(2, 64, 160)
    ufw.checksum(0, 2, 62)
    cipher(ufw.bytes, 0, 64, 0xffff)
    ufw.copy(1024, flash.bytes)
    ufw.copy(loader_at, loader)
    output = bytearray(total + 20)
    destination = 0
    for source in range(total):
        output[destination] = ufw.bytes[source]
        if source < 940 and (source + 1) % 47 == 0:
            index = source // 47
            destination += 1
            output[destination] = (ord(product[index]) + index + 1) & 0xff if index < len(product) else 125
        destination += 1
    return bytes(output)


def package_create(app, resources, output, user_bytes, product):
    if not product or not re.fullmatch(r'FM-1_8[0-9]{6}', product):
        fail('Invalid FM1 update identity.')
    if not app or len(app) > FLASH_LIMIT - USER_SIZE - BT_SIZE - MINIMUM_VM - AREA - APP_DATA - 383 - 64 or user_bytes != USER_SIZE:
        fail('Application does not fit the FM1 layout.')
    image_size = round_up(AREA + APP_DATA + len(app) + 383 + 64, 4096)
    user_at = FLASH_LIMIT - USER_SIZE
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MINIMUM_VM:
        fail('Insufficient FM1 settings space.')
    image = build_flash(app, resources, image_size, vm_end)
    Path(output).write_bytes(wrap(image, (resources / 'ota.bin').read_bytes(), product))
    return {'flash_bytes': 1048576, 'board_access': False, 'fits': True, 'packager': 'source',
            'FLASH_BIN_SIZE': image_size, 'FLASH_NEED_SIZE': FLASH_LIMIT, 'VM_START_ADDR': image_size, 'VM_END_ADDR': vm_end,
            'VM_REAL_SIZE': vm_end - image_size, 'BTIF_RESERVED_START': vm_end, 'BTIF_RESERVED_END': user_at,
            'USER_RESERVED_START': user_at, 'USER_RESERVED_END': FLASH_LIMIT, 'vm_minimum_bytes': MINIMUM_VM,
            'vm_extra_gap_bytes': vm_end - image_size - MINIMUM_VM, 'btif_bytes': BT_SIZE, 'user_bytes': USER_SIZE}


def package_firmware(tools, product):
    generated = BUILD / 'generated'
    elf = BUILD / 'sdk.elf'
    before = sha256(elf)
    output = generated / 'FM1_OFFICIAL_OPEN.fwsc'
    parts = ('text', 'data', 'ram0_data', 'cache_ram_data')
    complete = False
    try:
        if struct.unpack_from('<I', elf.read_bytes(), 24)[0] != 0x02000120:
            fail('Package entry must match the verified FM1 firmware entry.')
        image = bytearray()
        for section in parts:
            run(tools['objcopy'], ['-O', 'binary', '-j', '.' + section, elf, section + '.bin'], generated)
            image += (generated / (section + '.bin')).read_bytes()
        layout = package_create(bytes(image), SDK / 'tools' / 'pack', output, VARIANT['user_bytes'], product)
        if not output.exists() or sha256(elf) != before:
            fail('Firmware packaging did not produce a matching image.')
        write_json(BUILD / 'package-report.json', layout)
        print(f'Offline package fits 1 MiB: image {layout["FLASH_BIN_SIZE"]}, VM {layout["VM_REAL_SIZE"]}, '
              f'user {layout["user_bytes"]} bytes')
        complete = True
    finally:
        for section in parts:
            (generated / (section + '.bin')).unlink(missing_ok=True)
        if not complete:
            output.unlink(missing_ok=True)


# --- Build.ps1 -Package web step and tools/Build-Web.ps1 port -------------
STOCK_SHA256 = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a'
STOCK_LOADER_SHA256 = 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541'


def build_simulator(wasi_sdk, sim_bank=None):
    compiler = wasi_sdk / 'bin' / 'clang'
    sysroot = wasi_sdk / 'share' / 'wasi-sysroot'
    if not compiler.is_file() or not sysroot.is_dir():
        fail('Set --wasi-sdk to an extracted WASI SDK 27 directory.')
    if not (ROOT / 'web' / 'engine' / 'Simulator.c').is_file():
        fail('Missing browser sources. Download the complete source tree.')
    inputs = {}
    for base in ('app', 'web'):
        for path in visible_files(ROOT / base):
            name = path.relative_to(ROOT).as_posix()
            if not name.startswith('app/board/') and path.suffix in ('.c', '.h', '.inc'):
                inputs[name] = sha256(path)
    for name in ['tools/Build-Web.ps1'] + ['sdk/include/' + p.name for p in sorted((SDK / 'include').glob('*.h'))]:
        inputs[name] = sha256(ROOT / name)
    names = sorted(inputs)  # ordinal, as Build-Web.ps1
    out = ROOT / 'build' / 'web'
    out.mkdir(parents=True, exist_ok=True)
    bank_inc = out / 'sim_bank.inc'
    bank_inc.unlink(missing_ok=True)
    hashed = names
    if sim_bank:
        # A speech bank preloaded into the simulator's flash, which the
        # SDK-pinned page has no way to load.
        bank = sim_bank.read_bytes()
        if bank[:4] != b'HSTS' or len(bank) > 466944:
            fail(f'{sim_bank} is not a station speech bank.')
        rows = (','.join(str(b) for b in bank[i:i + 32]) for i in range(0, len(bank), 32))
        write_text(bank_inc, 'static const unsigned char sim_bank[] = {\n' + ',\n'.join(rows) + '\n};\n')
        inputs['sim_bank'] = sha256(sim_bank)
        hashed = sorted(inputs)
    source_hash = hashlib.sha256('\n'.join(f'{n}:{inputs[n]}' for n in hashed).encode('utf-8')).hexdigest()
    wasm = out / 'simulator.wasm'
    wasm.unlink(missing_ok=True)
    options = ['--sysroot=' + str(sysroot), '-std=c11', '-O2', '-fwrapv', '-fno-fast-math', '-ffp-contract=off',
               '-DFM1_HOST_TEST', '-DFM1_OPEN_WEB', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-array-parameter', '-mexec-model=reactor']
    options += ['-I' + str(ROOT / n) for n in ('web/engine', 'sdk/include', 'app/synth', 'app')]
    if sim_bank:
        options += ['-DFM1_SIM_BANK', '-I' + str(out)]
    options += [str(ROOT / n) for n in names if n.endswith('.c') and not n.startswith('sdk/')]
    options += ['-Wl,--strip-all', '-Wl,-z,stack-size=131072', '-Wl,--initial-memory=4194304',
                '-Wl,--max-memory=33554432', '-o', str(wasm)]
    print('Compile the browser application')
    run(compiler, options, ROOT)
    return wasm, source_hash


def build_web(wasi_sdk, source_url, release_version, product, sim_bank=None):
    import base64, html
    if not re.match(r'https://[^\s@/]+/', source_url or '') or re.search(r'[\r\n]', source_url):
        fail('--source-url must be an HTTPS source URL without credentials.')
    output = ROOT / 'build' / 'official-web' / 'FM1-Official-Open-Source.html'
    sums = output.parent / 'SHA256SUMS.txt'
    output.unlink(missing_ok=True)
    sums.unlink(missing_ok=True)
    package_file = BUILD / 'generated' / 'FM1_OFFICIAL_OPEN.fwsc'
    package_bytes = package_file.read_bytes()
    stock_file = SDK / 'firmware' / 'FM-1_015.fwsc'
    if not stock_file.is_file() or sha256(stock_file) != STOCK_SHA256:
        fail('Matching SDK original V15 recovery firmware is missing or invalid.')
    stock_bytes = stock_file.read_bytes()
    build_id = json.loads((BUILD / 'build-identity.json').read_text())['build_id']
    stock = {'schema': 1, 'kind': 'stock', 'product': 'FM-1_015', 'profile': 'M-VAVE Original V15', 'family': 7,
             'release_version': 'V15', 'build_id': STOCK_SHA256, 'sha256': STOCK_SHA256, 'bytes': len(stock_bytes),
             'loader_sha256': STOCK_LOADER_SHA256, 'data': base64.b64encode(stock_bytes).decode()}
    package = {'schema': 1, 'product': product, 'profile': PROFILE, 'family': VARIANT['update_family'],
               'release_version': release_version, 'build_id': build_id, 'sha256': sha256(package_file),
               'bytes': len(package_bytes), 'loader_sha256': sha256(SDK / 'tools' / 'pack' / 'ota.bin'),
               'hardware_validated': False, 'data': base64.b64encode(package_bytes).decode(), 'stock': stock}
    wasm, source_hash = build_simulator(wasi_sdk, sim_bank)
    simulation = {'schema': 1, 'release_version': release_version, 'sha256': sha256(wasm), 'source_sha256': source_hash,
                  'sample_bank': 'synthesized-demo', 'data': base64.b64encode(wasm.read_bytes()).decode()}
    notices = [f'APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate '
               f'redistribution terms in THIRD_PARTY.md. Source: {source_url}',
               'FM-1 V15 recovery firmware: https://www.m-vave.com/download (PC Firmware / FM-1 V15). Distributed by '
               'M-VAVE under Apache-2.0. Retain applicable licenses and notices; components with separate licenses '
               'retain their terms. See THIRD_PARTY.md.']
    for name in ('THIRD_PARTY.md', 'LICENSE.md'):
        notice = (ROOT / name).read_text(encoding='utf-8')
        if name == 'THIRD_PARTY.md' and not notice.startswith('# Third-party notices'):
            fail('Use the public distribution notices.')
        notices.append(f'\n===== {name} =====\n' + notice)
    compact = {'separators': (',', ':'), 'ensure_ascii': False}
    replacements = {
        '/*FM1_SOURCE_URL*/': html.escape(source_url),
        '/*FM1_PAYLOAD*/': 'const FM1_PACKAGE = ' + json.dumps(package, **compact) + ';',
        '/*FM1_SIMULATOR*/': 'const FM1_SIMULATOR = ' + json.dumps(simulation, **compact) + ';',
        '/*FM1_I18N*/': (SDK / 'I18n.js').read_text(encoding='utf-8'),
        '/*FM1_SCRIPT*/': (SDK / 'WebUpdate.js').read_text(encoding='utf-8'),
        '/*FM1_SIM_SCRIPT*/': (SDK / 'Simulator.js').read_text(encoding='utf-8'),
        '/*FM1_SAMPLE_SCRIPT*/': (SDK / 'Samples.js').read_text(encoding='utf-8'),
        '/*FM1_NOTICES*/': html.escape('\n'.join(notices)),
    }
    web = (SDK / 'Update.html.in').read_text(encoding='utf-8')
    for key, value in replacements.items():
        web = web.replace(key, value)
    if re.search(r'/\*FM1_[A-Z_]+\*/', web):
        fail('Unexpanded HTML placeholder.')
    # Ours, appended after the SDK-pinned template: sets the simulator's clock.
    web += '\n<script>\n' + (ROOT / 'tools' / 'sim-clock.js').read_text(encoding='utf-8') + '</script>\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    write_text(output, web)
    collector = output.parent / 'collector.html'
    collector.write_bytes((ROOT / 'tools' / 'collector.html').read_bytes())
    write_text(sums, f'{sha256(output)}  {output.name}\n{sha256(collector)}  {collector.name}\n')
    write_json(BUILD / 'web-package-report.json', {
        'integrated': True, 'firmware_sha256': package['sha256'], 'web_sha256': sha256(output),
        'release_version': release_version, 'build_id': build_id, 'simulator_sha256': simulation['sha256'],
        'simulator_source_sha256': source_hash, 'simulator_bytes': wasm.stat().st_size,
        'hardware_validated': False, 'board_access': False})
    print(f'Integrated FM1 workbench: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--toolchain', default=os.environ.get('FM1_PI32_TOOLCHAIN_LINUX'),
                        help='extracted jieli-linux-toolchains directory (contains common/ and pi32v2/)')
    parser.add_argument('--web', action='store_true',
                        help='also build the browser update page with this firmware embedded')
    parser.add_argument('--wasi-sdk', default=os.environ.get('FM1_WASI_SDK'),
                        help='extracted WASI SDK 27 directory, for --web')
    parser.add_argument('--source-url', default=os.environ.get('FM1_SOURCE_URL'),
                        help='HTTPS URL of the matching source, shown in the page notices, for --web')
    parser.add_argument('--sim-bank',
                        help='speech bank (tools/speech.py bank) to preload into the simulator, for --web')
    args = parser.parse_args()
    if not args.toolchain:
        fail('Set --toolchain or FM1_PI32_TOOLCHAIN_LINUX to the extracted JieLi Linux toolchain.')
    if args.web and not args.wasi_sdk:
        fail('Set --wasi-sdk or FM1_WASI_SDK for --web.')
    toolchain = Path(args.toolchain).resolve()

    # Invalidate stale successful output before validating build inputs.
    BUILD.mkdir(parents=True, exist_ok=True)
    write_text(BUILD / 'firmware-report.json', '{"passed":false}')
    write_text(BUILD / 'package-report.json', '{"board_access":false,"fits":false}')
    write_text(BUILD / 'web-package-report.json', '{"integrated":false,"board_access":false}')
    for name in ('sdk.elf', 'sdk.map', 'build-identity.json', 'compile_commands.json', 'link-arguments.json',
                 'generated/FM1_OFFICIAL_OPEN.fw', 'generated/FM1_OFFICIAL_OPEN.fwsc'):
        (BUILD / name).unlink(missing_ok=True)
    for name in ('official-web/FM1-Official-Open-Source.html', 'official-web/collector.html',
                 'official-web/SHA256SUMS.txt'):
        (ROOT / 'build' / name).unlink(missing_ok=True)

    version_h = (ROOT / 'app' / 'version.h').read_text()
    m = re.search(r'(?m)^#define FM1_RELEASE_VERSION "(V(?:0[1-9]|[1-9][0-9]{1,2}))"\s*$', version_h)
    if not m:
        fail('Set FM1_RELEASE_VERSION in app/version.h.')
    release_version = m.group(1)
    m = re.search(r'(?m)^#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"\s*$', version_h)
    if not m:
        fail('Set a compatible FM1_UPDATE_PRODUCT in app/version.h.')
    product = m.group(1)
    print(f'Build FM1 Official Open {release_version} (Linux toolchain)')

    manifest = json.loads((SDK / 'manifest.json').read_text(encoding='utf-8-sig'))
    if manifest.get('abi') != 131072:
        fail('This application requires the matching 128-frame SDK.')
    tools = verify_toolchain(toolchain, manifest)
    synth = json.loads((SDK / 'synth.json').read_text(encoding='utf-8-sig'))
    if (synth.get('schema') != 4 or synth.get('api') != 8 or synth.get('kernel_abi') != 3 or
            synth.get('block_frames') != 128 or synth.get('sample_rate') != 44100 or
            synth.get('sdk_block_sha256') != sha256(SDK / 'include' / 'fm1_block.h') or
            synth.get('native_sha256') != sha256(SDK / 'lib' / 'libfm1_engine.a')):
        fail('Native DSP kernels and SDK audio contracts do not match. Install a matching SDK package.')
    if not re.search(r'(?m)^#define FM1_DSP_ABI 3U\s*$', (ROOT / 'app' / 'synth' / 'engine.h').read_text()):
        fail('Unsupported native DSP ABI.')
    for name, expected in (synth.get('portable_contracts') or {}).items():
        if not re.fullmatch(r'[A-Za-z0-9_]+\.inc', name) or expected != sha256(ROOT / 'app' / 'synth' / name):
            fail('Fixed DSP coefficients differ from the native kernels. Matching kernels are required for this coefficient change.')
    if not synth.get('sram_required') or not synth.get('sram_optional'):
        fail('Native DSP SRAM contract is missing.')
    for name in list(synth['sram_required']) + list(synth['sram_optional']):
        if not re.fullmatch(r'[A-Za-z_][\w.$]*', name):
            fail('Invalid DSP SRAM symbol.')
    for name, expected in manifest['files'].items():
        full = (ROOT / name).resolve()
        if SDK not in full.parents or sha256(full) != expected:
            fail(f'SDK payload changed: {name}')
    for path in SDK.rglob('*'):
        relative = path.relative_to(ROOT).as_posix()
        if path.is_file() and relative != 'sdk/manifest.json' and relative not in manifest['files']:
            fail(f'Unexpected SDK file: {relative}')

    include = ['sdk/include', 'sdk/include/libc', 'app/synth', 'sdk/config', 'app', 'app/board']
    synth_dir = ROOT / 'app' / 'synth'
    sources = sorted((p.relative_to(ROOT).as_posix() for p in visible_files(ROOT / 'app')
                      if p.suffix in ('.c', '.S') and synth_dir not in p.parents), key=windows_sort_key)
    extra = ['groove_sequence_render', 'groove_sequence_clock', 'groove_sequence_position', 'groove_sequence_transport',
             'fm1_sea_decode_groups', 'fm1_codec_sea_mono_s16_run'] + list(synth['sram_required']) + [
        'synth_dx7_render', 'synth_dx7_midi', 'synth_dx7_key', 'fm1_dx7_panic', 'fm_exp2_fixed', 'bass303_bank_render', 'bass303_bank_key',
        'bass303_bank_set', 'drum808_kit_render', 'drum808_kit_trigger', 'psg_render', 'psg_key', 'psg_midi', 'psg_panic', 'psg_set', 'psg_get',
        'pluck_render', 'pluck_key', 'pluck_midi', 'pluck_panic', 'pluck_set', 'pluck_get', 'fm1_mix128', 'fm1_mix_mono_stereo128',
        'fm1_bus_output128', 'fm1_master_mix128', 'fm1_master_gain', 'fm1_declick128', 'fm1_render_begin', 'fm1_render_end',
        'synth_dx7_active', 'bass303_bank_active', 'drum808_kit_active', 'psg_active', 'pluck_active', 'sampledrum_active',
        'synth_dx7_limit', 'psg_limit', 'pluck_limit', 'sampledrum_limit', 'bass303_advance_idle', 'fm1_send128', 'fm1_fx_render',
        'fm1_fx_clear', 'fm1_fx_active', 'sampledrum_render', 'sampledrum_trigger', 'sampledrum_panic', 'psg_active_chips']
    objects_dir = BUILD / 'obj'
    objects_dir.mkdir(exist_ok=True)
    (BUILD / 'generated').mkdir(exist_ok=True)
    if (ROOT / 'build') not in objects_dir.resolve().parents:
        fail('Invalid object directory.')
    for path in objects_dir.iterdir():
        if path.is_file():
            path.unlink()
    flags = ['-target', 'pi32v2', '-integrated-as', '-mcpu=r3', '-mfprev1', '-Wuninitialized',
             '-Wno-invalid-noreturn', '-fno-common', '-flto', '-fallow-pointer-null', '-fprefer-gnu-section',
             '-femulated-tls', '-Wno-shift-negative-value', '-Wframe-larger-than=2560', '-fms-extensions',
             '-fno-unwind-tables', '-ffunction-sections', '-fdata-sections', '-fmessage-length=0',
             '-cl-single-precision-constant', '-mllvm', '-pi32v2-large-program=true', '-D__GCC_PI32V2__', '-O2']
    flags += ['-I' + str(ROOT / d) for d in include]

    objects, commands = [], []
    for index, source in enumerate(sources):
        output = f'obj/{index}_' + re.sub(r'[^a-zA-Z0-9._-]', '_', source) + '.obj'
        options = list(flags)
        # This PI32 LTO plugin can discard application definitions when a
        # native SDK object has weak defaults. Keep user entry units native.
        if source in ('app/screen.c', 'app/transfer.c', 'app/midi_router.c'):
            options.append('-fno-lto')
        if source == 'app/board/panel.c':
            options += ['-fno-lto', '-fno-pic']
        options += ['-fwrapv', '-fno-fast-math', '-ffp-contract=off']
        if source in ('app/project.c', 'app/transfer.c', 'app/samples.c', 'app/screen.c', 'app/presets.c',
                      'app/midi_router.c', 'app/fm_factory.c', 'app/fm_library.c'):
            options.append('-Oz')
        if source == 'app/screen.c':
            options.append('-fno-inline-functions')
        arguments = options + ['-MMD', '-MF', output + '.d', '-c', str(ROOT / source), '-o', output]
        print(f'Compile {source}')
        run(tools['clang'], arguments, BUILD)
        objects.append(output)
        commands.append({'directory': str(BUILD), 'file': str(ROOT / source), 'output': str(BUILD / output),
                         'arguments': [str(tools['clang'])] + arguments})

    # The device reports this source/SDK build identity after a browser update.
    identity_inputs = [PROFILE, sha256(SDK / 'manifest.json')]
    for path in sorted(visible_files(ROOT / 'app'), key=lambda p: windows_sort_key(p.relative_to(ROOT).as_posix())):
        identity_inputs.append(path.relative_to(ROOT).as_posix() + ':' + sha256(path))
    build_identity = hashlib.sha256('\n'.join(identity_inputs).encode('utf-8')).hexdigest()
    if VARIANT['update_family'] != 7 or VARIANT['user_bytes'] < 4096 or VARIANT['user_bytes'] % 4096:
        fail('Invalid fixed firmware layout.')
    id_bytes = ','.join('0x' + build_identity[i:i + 2] for i in range(0, 64, 2))
    write_text(BUILD / 'generated' / 'build_identity.c',
               '#include "layout.h"\n'
               f'const struct fm1_firmware_layout fm1_layout = {{{VARIANT["user_bytes"]}U,{VARIANT["update_family"]}U}};\n'
               f'const unsigned char fm1_build_id[32] = {{{id_bytes}}};\n'
               f'const unsigned char fm1_update_identity[27] = "{product}";\n')
    run(tools['clang'], flags + ['-c', 'generated/build_identity.c', '-o', 'obj/build_identity.obj'], BUILD)
    objects.append('obj/build_identity.obj')
    write_json(BUILD / 'build-identity.json', {'profile': PROFILE, 'family': 'release', 'release_version': release_version,
                                               'build_id': build_identity})
    linker = ((SDK / 'config' / 'linker.ld.in').read_text(encoding='utf-8')
              .replace('@SBC_DECODER@', 'sbc_decoder\n').replace('@ACL_TX_EXTRA@', ' + 1024').replace('@ACL_RX_EXTRA@', ' + 4096'))
    write_text(BUILD / 'generated' / 'linker', linker)
    link = ['--plugin', str(tools['gold']), '-o', 'sdk.elf'] + objects + [
        '-flto', '--plugin-opt=-pi32v2-always-use-itblock=false', '--plugin-opt=-enable-ipra=true',
        '--plugin-opt=-pi32v2-merge-max-offset=4096', '--plugin-opt=-pi32v2-enable-simd=true', '--plugin-opt=mcpu=r3',
        '--plugin-opt=-global-merge-on-const', '--plugin-opt=-inline-threshold=5', '--plugin-opt=-inline-max-allocated-size=32',
        '--plugin-opt=-inline-normal-into-special-section=true', '--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar',
        '--plugin-opt=-pi32v2-enable-rep-memop', '--plugin-opt=-emulated-tls', '--sort-common',
        '--plugin-opt=-pi32v2-large-program=true', '--plugin-opt=-pi32v2-csync-before-rti=true', '--start-group',
        '--whole-archive', '../../' + VARIANT['frozen'], '--no-whole-archive',
        '../../sdk/lib/libc.a', '../../sdk/lib/libcompiler_rt.a',
        '--end-group', '--gc-sections', '-Tgenerated/linker', '-M=sdk.map', '--undefined=connectivity_wl82_bluetooth_start',
        '--undefined=audio_app_bluetooth_open', '--undefined=bt_get_local_name', '--undefined=bt_get_mac_addr']
    write_json(BUILD / 'compile_commands.json', commands)
    write_json(BUILD / 'link-arguments.json', link)
    print(f'Link {PROFILE} using prebuilt FM1 SDK')
    # JieLi: linking may open many files at once.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, min(8192, hard)), hard))
    run(tools['ld'], link, BUILD)
    test_firmware(tools, extra, synth)
    package_firmware(tools, product)
    print(f'Firmware: {BUILD / "generated" / "FM1_OFFICIAL_OPEN.fwsc"}')
    if args.web:
        build_web(Path(args.wasi_sdk).resolve(), args.source_url, release_version, product,
                  Path(args.sim_bank).resolve() if args.sim_bank else None)


if __name__ == '__main__':
    try:
        main()
    except BuildError as error:
        sys.exit(f'error: {error}')
