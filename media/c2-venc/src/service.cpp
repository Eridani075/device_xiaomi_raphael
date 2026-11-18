/*
 * Codec2 HAL service for the sm8150 V4L2 video encoder.
 *
 * Codec2Client uses HIDL on Raphael. The framework compatibility matrix
 * permits "default2" alongside the existing QTI and Dolby instances.
 */

#include <csignal>
#include <cstdlib>
#include <string>

#include <C2Component.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
#include <minijail.h>

#undef LOG_TAG
#define LOG_TAG "sm8150-c2-venc-service"

namespace android {
// Defined in C2VencStore.cpp. Declared here rather than in a header because
// this service and the store are a single module and nothing else needs it.
std::shared_ptr<C2ComponentStore> CreateC2VencStore();
}  // namespace android

// Absolute on-device path of the prebuilt_etc module
// "android.hardware.media.c2-default-seccomp_policy"
// (frameworks/av/media/codec2/hal/services/Android.bp). The service installs
// this policy through its Soong required list.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// The service installs this ARM64 extension with its base policy.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "sm8150-c2-venc-extended-seccomp_policy";

// Matches AOSP's vendor service. Extra threads are needed because a stacked
// IPC sequence can alternate binder and hwbinder calls (b/35283480), and
// because a blocking operation on one codec instance must not stall another.
static constexpr int kThreadCount = 8;

int main(int /*argc*/, char ** /*argv*/) {
    ALOGD("sm8150 Codec2 encoder service starting...");

    // A dead client must not take the HAL down with it.
    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // vndbinder, not the default /dev/binder: this is a vendor process and the
    // buffer pool traffic it does is vendor-to-vendor.
    android::ProcessState::initWithDriver("/dev/vndbinder");
    android::ProcessState::self()->startThreadPool();
    android::hardware::configureRpcThreadpool(kThreadCount, true /*callerWillJoin*/);

    {
        using ::android::hardware::media::c2::V1_2::IComponentStore;
        using ::android::hardware::media::c2::V1_2::utils::ComponentStore;

        // ComponentStore's constructor calls SetPreferredCodec2ComponentStore(),
        // which is what makes the allocator usage mapping in C2VencStore's
        // Interface take effect for this process.
        android::sp<IComponentStore> store = new ComponentStore(android::CreateC2VencStore());

        constexpr char const *kInstanceName = "default2";
        if (store->registerAsService(kInstanceName) != android::OK) {
            ALOGE("cannot register IComponentStore as \"%s\"", kInstanceName);
            return EXIT_FAILURE;
        }
        ALOGD("IComponentStore registered as \"%s\"", kInstanceName);
    }

    android::hardware::joinRpcThreadpool();
    ALOGD("sm8150 Codec2 encoder service shutdown.");
    return 0;
}
