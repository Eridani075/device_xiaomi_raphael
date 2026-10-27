#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024-2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import os

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
    (
        'libwfdconfigutils',
        'vendor.qti.qspmhal@1.0',
    ): lib_fixup_remove,
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
