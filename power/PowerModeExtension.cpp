// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <unistd.h>

#include <aidl/android/hardware/power/Mode.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

namespace aidl::google::hardware::power::impl::pixel {

using ::aidl::android::hardware::power::Mode;

namespace {
constexpr char kDoubleTapToWakePath[] = "/sys/touchpanel/double_tap";
}  // namespace

bool isDeviceSpecificModeSupported(Mode type, bool* supported) {
    if (type != Mode::DOUBLE_TAP_TO_WAKE) {
        return false;
    }
    *supported = true;
    return true;
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    if (type != Mode::DOUBLE_TAP_TO_WAKE) {
        return false;
    }

    // Apply the setting directly; this switch is independent of perf hints.
    const ::android::base::unique_fd fd(
            TEMP_FAILURE_RETRY(open(kDoubleTapToWakePath, O_WRONLY | O_CLOEXEC)));
    if (fd.get() < 0 || !::android::base::WriteStringToFd(enabled ? "1" : "0", fd.get())) {
        PLOG(ERROR) << "Failed to set double tap to wake to " << enabled;
    }
    return true;
}

}  // namespace aidl::google::hardware::power::impl::pixel
