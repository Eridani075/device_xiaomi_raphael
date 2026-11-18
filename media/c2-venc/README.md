# SM8150 Codec2 encoder service

`sm8150-c2-venc-service` exposes `c2.sm8150.avc.encoder` and
`c2.sm8150.hevc.encoder` through its HIDL 1.2 `IComponentStore/default2`.
The store advertises rank 512 by default. Raphael 5.4 packages the ARM64
service in all build variants and uses rank 128 in its vendor codec XML,
ahead of QTI's rank 256 for MIME-based selection.

The module is built in the Raphael Soong namespace on `cnb-5.4`. Use the
normal SM8150 media checkout with this device tree. The build also uses
the SM8250 V4L2 headers and the selected 5.4 kernel's exported headers.

## Buffer handling

- The component uses the mapped graphic buffer's layout and row stride
  to copy each input into a Venus-aligned NV12 slot. Packed RGBA uses
  libyuv for conversion. A slot can be reused after the driver returns it.
- The sm8250 `venc_dev` engine queues capture buffers independently of
  input and drives SM8150's V4L2 encoder. Codec initialization data travels
  as a Codec2 config update attached to the next encoded frame.
- Driver callbacks stage results for the Codec2 work looper, which completes
  each work item after pending registration. End-of-stream waits for the
  driver to drain and fails if it times out or still has outstanding frames.
- The service uses the platform `mediacodec` domain and ARM64 seccomp policy
  with an extension for the hardware encoder. The runtime dependencies
  include the platform Codec2 libraries, gralloc, ION, and the VIDC driver
  and firmware.

## Validation

On Raphael 5.4, explicit AVC screen capture produced a playable MP4. AVC
and HEVC each returned all 60 submitted graphic frames and output EOS in
a MediaCodec test. Use the default Screen Recorder entry point to validate
the device's codec ordering and audio and video capture together.

The component targets 8-bit graphic buffers. ByteBuffer input, runtime
bitrate changes, and sync-frame requests remain outside this implementation.

## Source

Restored from a local snapshot of
`erg-raphael-5-4/android_hardware_qcom_media_sm8150` at
`4a20720839baa1a880d2dc8585db2b0e68bbb4c1`. The vendored engine retains
its Linux Foundation BSD 3-Clause notices. See [engine/README.md](engine/README.md).
The source repository's [NOTICE](NOTICE) is included here.
The Codec2 wrapper retains its source attribution. Redistribution terms
for the wrapper are open for verification.
