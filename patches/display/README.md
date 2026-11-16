# SM8150 display fallback for Raphael on Linux 5.4

The normal build uses unmodified LineageOS SM8150 display sources for gralloc
and the other common modules, the upstream HIDL 2.4 composer service, and the
matched HWC/SDM/DRM prebuilts listed in `proprietary-files-platform.txt`.
Extract those display files from the validated mixed vendor image
(`vendor-5.4-legacy-display-test.img`, SHA-256
`5587920b7c04bb07d50c81c243b0feb505cdfea073c17c77e09647933d826f62`).
Its ARM64 display libraries came from the signed 20261031 4.14 build; its ARM
libraries are unchanged from the 20261111 5.4 baseline. Keep the matched
libraries together. The `pps` override and both panel-port configs remain in
the device tree.

This mailbox is kept as a fallback. Restore the source module selections in
both the device and vendor trees before applying it; do not apply it over the
default prebuilt build.

Pin `hardware/qcom-caf/sm8150/display` to LineageOS's
`lineage-24.0-caf-sm8150` commit
`6e9e061073de8c0ae6568cb71a168cc6a2bad5d9` in the local manifest.
After syncing that project to the pin, apply the mailbox once in a clean
checkout from the Android source root:

```bash
patch="$PWD/device/xiaomi/raphael/patches/display/linux-5.4.mbox"
display=hardware/qcom-caf/sm8150/display
test "$(git -C "$display" rev-parse HEAD)" = 6e9e061073de8c0ae6568cb71a168cc6a2bad5d9 &&
test -z "$(git -C "$display" status --porcelain)" &&
git -C "$display" switch -c cnb-5.4-los-patch &&
git -C "$display" am --no-3way "$patch" &&
test "$(git -C "$display" rev-parse 'HEAD^{tree}')" = 2ee411cef3f0855f4a42ee7cadf5eb203aae72d8
```

The patch builds SDM and its SDE backend together against the Raphael kernel's
generated headers. It does not alter the device's composer service override.
When updating the LineageOS pin, replay and validate the patch first; it is
not reapplied automatically by Repo or during the build.
