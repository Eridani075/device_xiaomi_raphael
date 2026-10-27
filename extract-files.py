#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024-2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import os
import xml.etree.ElementTree as ET

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)
from extract_utils.tools import (
    patchelf_version_path_map,
)
from extract_utils.utils import (
    run_cmd,
)

# High-speed (slow motion) encoder profiles for the back camera; the stock
# media_profiles only carries them for the other camera ids.
CAMERA0_HIGHSPEED_PROFILES = '''    <EncoderProfile quality="highspeed1080p" fileFormat="mp4" duration="30">
      <Video codec="h264"
             bitRate="42000000"
             width="1920"
             height="1080"
             frameRate="120" />

      <!-- audio setting is ignored -->
      <Audio codec="aac"
             bitRate="96000"
             sampleRate="48000"
             channels="1" />
    </EncoderProfile>

    <EncoderProfile quality="highspeedhigh" fileFormat="mp4" duration="30">
      <Video codec="h264"
             bitRate="24000000"
             width="1280"
             height="720"
             frameRate="240" />

      <!-- audio setting is ignored -->
      <Audio codec="aac"
             bitRate="96000"
             sampleRate="48000"
             channels="1" />
    </EncoderProfile>

    <EncoderProfile quality="highspeed720p" fileFormat="mp4" duration="30">
      <Video codec="h264"
             bitRate="24000000"
             width="1280"
             height="720"
             frameRate="240" />

      <!-- audio setting is ignored -->
      <Audio codec="aac"
             bitRate="96000"
             sampleRate="48000"
             channels="1" />
    </EncoderProfile>

    <EncoderProfile quality="highspeed480p" fileFormat="mp4" duration="30">
      <Video codec="h264"
             bitRate="12000000"
             width="640"
             height="480"
             frameRate="240" />

      <Audio codec="aac"
             bitRate="156000"
             sampleRate="48000"
             channels="2" />
    </EncoderProfile>

'''
CAMERA0_END = '    </CamcorderProfiles>\n    <!-- Front Camera -->'


def mpbase_use_padding_shim(ctx, file, file_path, *args, **kwargs):
    # Route libmpbase's malloc/realloc through libmpbase_shim, which pads
    # large allocations (the refocus engine reads past their end).
    syms = f'{file_path}.syms'
    with open(syms, 'w') as f:
        f.write('malloc mpbase_shim_malloc\nrealloc mpbase_shim_realloc\n')
    run_cmd([patchelf_version_path_map['0_18'], '--rename-dynamic-symbols', syms, file_path])
    os.remove(syms)


def adapt_stock_qdcm_calib(ctx, file, file_path, *args, **kwargs):
    # Map stock MIUI 12.5 QDCM calibration modes to the attribute combinations
    # expected by HWCColorMode::PopulateColorModes() in the QCOM Display HAL,
    # dropping MIUI-only game/video filter modes (GM_*, video*) and <Luts>.
    root = ET.parse(file_path).getroot()
    disp_modes = root.find('Disp_Modes')
    if disp_modes is None or disp_modes.get('NumModes') == '6':
        return

    by_id = {m.get('ModeID'): m for m in disp_modes.findall('Mode')}
    # (stock_id, new_id, color_gamut, dynamic_range, picture_quality)
    target_map = [
        ('40', '0', 'srgb', 'sdr', 'enhanced'),     # Boosted (sRGB Enhanced)
        ('51', '1', 'dcip3', 'sdr', 'enhanced'),    # Adaptive (DCI-P3 Enhanced)
        ('01', '2', 'native', 'sdr', None),         # Saturated (Panel Native)
        ('3', '3', 'srgb', 'sdr', 'standard'),      # Natural (sRGB Colorimetric)
        ('36', '4', 'dcip3', 'sdr', 'standard'),    # Natural (DCI-P3 Colorimetric)
        ('39', '5', 'dcip3', 'hdr', 'standard'),    # HDR10 / HLG
    ]

    selected = []
    for stock_id, new_id, gamut, dr, pq in target_map:
        m = by_id[stock_id]
        m.set('ModeID', new_id)
        m.set('DynamicRange', dr)
        m.set('ColorGamut', gamut)
        if pq is not None:
            m.set('PictureQuality', pq)
        else:
            m.attrib.pop('PictureQuality', None)
        selected.append(m)

    for m in list(disp_modes):
        disp_modes.remove(m)
    for m in selected:
        disp_modes.append(m)

    disp_modes.set('NumModes', str(len(selected)))
    disp_modes.set('DefaultMode', '0')

    for luts in root.findall('Luts'):
        root.remove(luts)

    ET.indent(root, space='    ')
    xml_body = ET.tostring(root, encoding='unicode')
    header = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!--Copyright (c) 2015 Qualcomm Technologies, Inc. All Rights Reserved.\n'
        'Qualcomm Technologies Proprietary and Confidential.-->\n'
    )
    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(header + xml_body + '\n')


blob_fixups: blob_fixups_user_type = {
    'system/framework/WfdCommon.jar': blob_fixup()
        .apktool_patch('blob-patches/WfdCommon.patch'),
    'system_ext/lib64/libwfdservice.so': blob_fixup()
        .replace_needed('android.media.audio.common.types-V4-cpp.so', 'android.media.audio.common.types-V5-cpp.so'),
    'vendor/etc/media_profiles_V1_0.xml': blob_fixup()
        .regex_replace(CAMERA0_END, CAMERA0_HIGHSPEED_PROFILES + CAMERA0_END)
        .regex_replace('maxHFRMode="120"  />', 'maxHFRMode="240"  />'),
    'vendor/etc/msm_irqbalance.conf': blob_fixup()
        .regex_replace('IGNORED_IRQ=19,21,39', 'IGNORED_IRQ=19,21,38'),
    'vendor/etc/init/android.hardware.drm@1.3-service.widevine.rc': blob_fixup()
        .regex_replace(r'writepid.*', 'task_profiles ProcessCapacityHigh HighPerformance'),
    'vendor/etc/init/android.hardware.neuralnetworks@1.3-service-qti.rc': blob_fixup()
        .regex_replace(r'writepid.*', 'task_profiles NNApiHALPerformance'),
    'vendor/etc/init/init.batterysecret.rc': blob_fixup()
        .regex_replace('.*seclabel u:r:batterysecret:s0\n', ''),
    'vendor/etc/init/init.mi_thermald.rc': blob_fixup()
        .regex_replace('.*seclabel u:r:mi_thermald:s0\n', ''),
    'vendor/etc/init/vendor.qti.media.c2@1.0-service.rc': blob_fixup()
        .regex_replace(r'writepid.*', 'task_profiles ProcessCapacityHigh HighPerformance'),
    'vendor/etc/init/vendor.sensors.sscrpcd.rc': blob_fixup()
        .regex_replace(
            r'capabilities BLOCK_SUSPEND\n',
            'capabilities BLOCK_SUSPEND\n'
            '    # The fastrpc default search path. Set it so apps_std stops logging a\n'
            '    # failed getenv on every file it opens (the backlight, several times a second).\n'
            '    setenv ADSP_LIBRARY_PATH ";/vendor/lib/rfsa/adsp;/vendor/lib/rfsa/dsp;/vendor/dsp"\n',
        ),
    (
        'vendor/etc/qdcm_calib_data_samsung_ea8076_fhd_cmd_dsi_panel.xml',
        'vendor/etc/qdcm_calib_data_visionox_r66456_fhd_cmd_dsi_panel.xml',
    ): blob_fixup()
        .call(adapt_stock_qdcm_calib),
    'vendor/etc/seccomp_policy/atfwd@2.0.policy': blob_fixup()
        .add_line_if_missing('gettid: 1'),
    'vendor/etc/sensors/hals.conf': blob_fixup()
        .add_line_if_missing('sensors.udfps.so'),
    'vendor/lib/hw/audio.primary.raphael.so': blob_fixup()
        .binary_regex_replace(
            b'/vendor/lib/liba2dpoffload.so',
            b'liba2dpoffload_raphael.so\x00\x00\x00\x00',
        )
        .replace_needed('libaudioroute.so', 'libaudioroute-v34.so'),
    'vendor/lib/libaudioroute_ext.so': blob_fixup()
        .replace_needed('libaudioroute.so', 'libaudioroute-v34.so'),
    'vendor/lib64/camera/components/com.qti.node.watermark.so': blob_fixup()
        .add_needed('libpiex_shim.so'),
    (
        'vendor/lib64/libalLDC.so',
        'vendor/lib64/libalAILDC.so',
        'vendor/lib64/libalhLDC.so',
    ): blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    (
        'vendor/lib64/libarcsoft_dualcam_refocus_front.so',
        'vendor/lib64/libarcsoft_dualcam_refocus_rear_t.so',
        'vendor/lib64/libarcsoft_dualcam_refocus_rear_w.so',
    ): blob_fixup()
        .clear_symbol_version('remote_handle_close')
        .clear_symbol_version('remote_handle_invoke')
        .clear_symbol_version('remote_handle_open'),
    'vendor/lib64/libdpps.so': blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),
    'vendor/lib64/libhvx_proxy_stub.so': blob_fixup()
        .clear_symbol_version('remote_handle64_close')
        .clear_symbol_version('remote_handle64_invoke')
        .clear_symbol_version('remote_handle64_open')
        .clear_symbol_version('remote_register_dma_handle')
        .clear_symbol_version('remote_register_dma_handle_attr'),
    'vendor/lib64/libmpbase.so': blob_fixup()
        .call(mpbase_use_padding_shim)
        .add_needed('libmpbase_shim.so'),
    'vendor/lib64/libwvhidl.so': blob_fixup()
        .add_needed('libcrypto_shim.so'),
    'vendor/lib64/mediadrm/libwvdrmengine.so': blob_fixup()
        .add_needed('libcrypto_shim.so'),
}  # fmt: skip


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'libmmosal',
        'vendor.qti.hardware.fm@1.0',
        'vendor.qti.hardware.wifidisplaysession@1.0',
        'vendor.qti.imsrtpservice@3.0',
    ): lib_fixup_vendor_suffix,
    'vendor.qti.qspmhal@1.0': lib_fixup_remove,
}

namespace_imports = [
    'device/xiaomi/raphael',
    'hardware/qcom-caf/common/libqti-perfd-client',
    'hardware/qcom-caf/sm8150',
    'hardware/qcom-caf/wlan',
    'hardware/xiaomi',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/qcom/opensource/commonsys/display',
    'vendor/qcom/opensource/dataservices',
    'vendor/qcom/opensource/display',
]

module = ExtractUtilsModule(
    'raphael',
    'xiaomi',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

module.add_proprietary_file('proprietary-files-platform.txt')

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
