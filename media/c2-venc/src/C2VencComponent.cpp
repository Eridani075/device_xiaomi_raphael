/*
 * See C2VencComponent.h. The engine (venc_dev) owns all V4L2 behaviour; this
 * file is the translation layer between Codec2's work items and the engine's
 * OMX-shaped buffer calls.
 *
 * The poll thread collects encoded buffers. The Codec2 looper delivers them
 * after SimpleC2Component has registered the corresponding work.
 */

#include "C2VencComponent.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include <inttypes.h>
#include <linux/dma-buf.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libyuv.h>

#include <C2ComponentFactory.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <log/log.h>

#include <vidc/media/msm_media_info.h>

#include "video_encoder_device_v4l2.h"
// For struct venc_msg / VEN_MSG_* only; the OMX class it declares is unused.
#include "omx_video_base.h"

#undef LOG_TAG
#define LOG_TAG "C2VencComponent"

namespace android {

namespace {
constexpr char COMPONENT_NAME_AVC[] = "c2.sm8150.avc.encoder";
constexpr char COMPONENT_NAME_HEVC[] = "c2.sm8150.hevc.encoder";
// Spelled out rather than pulled from MediaDefs.h: they are the only symbols
// we wanted from libstagefright_foundation, and a string literal is not worth
// a shared-library dependency.
constexpr char MIME_AVC[] = "video/avc";
constexpr char MIME_HEVC[] = "video/hevc";
constexpr uint32_t kDefaultBitrate = 20000000;
// venc_dev's own default is 29; the driver refuses much larger GOPs.
constexpr uint32_t kMaxPFrames = 255;  // 20 Mbps, matches CamcorderProfile 1080p
constexpr auto kInputSlotTimeout = std::chrono::milliseconds(500);
// MediaCodecSource allows up to one second to finish a surface recording.
constexpr auto kEosTimeout = std::chrono::milliseconds(750);

bool ConvertRgbToNv12(const C2GraphicView &rgb, uint32_t width, uint32_t height,
                      uint8_t *dstY, int dstYStride, uint8_t *dstUV, int dstUVStride) {
    const auto crop = rgb.crop();
    const auto layout = rgb.layout();
    if (crop.width != width || crop.height != height ||
        (layout.type != C2PlanarLayout::TYPE_RGB && layout.type != C2PlanarLayout::TYPE_RGBA) ||
        layout.numPlanes < 3 || layout.rootPlanes != 1) {
        return false;
    }
    const auto data = rgb.data();
    const auto &red = layout.planes[C2PlanarLayout::PLANE_R];
    if (data == nullptr || data[C2PlanarLayout::PLANE_R] == nullptr || red.rowInc <= 0 ||
        (static_cast<uint64_t>(crop.left) + width) * 4 > static_cast<uint64_t>(red.rowInc)) {
        return false;
    }
    for (uint32_t i = C2PlanarLayout::PLANE_R; i <= C2PlanarLayout::PLANE_B; ++i) {
        const auto &plane = layout.planes[i];
        if (plane.colInc != 4 || plane.rowInc != red.rowInc || plane.bitDepth != 8 ||
            plane.allocatedDepth != 8 || plane.rightShift != 0 ||
            plane.colSampling != 1 || plane.rowSampling != 1 ||
            reinterpret_cast<uintptr_t>(data[i]) !=
                    reinterpret_cast<uintptr_t>(data[C2PlanarLayout::PLANE_R]) + i) {
            return false;
        }
    }
    const uint8_t *start = data[C2PlanarLayout::PLANE_R] +
            static_cast<size_t>(crop.top) * red.rowInc + static_cast<size_t>(crop.left) * 4;
    return libyuv::ABGRToNV12(start, red.rowInc, dstY, dstYStride, dstUV, dstUVStride,
                             static_cast<int>(width), static_cast<int>(height)) == 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------

class C2VencComponent::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper, Codec codec)
        : SimpleInterface<void>::BaseParams(helper,
                                            codec == Codec::HEVC ? COMPONENT_NAME_HEVC
                                                                 : COMPONENT_NAME_AVC,
                                            C2Component::KIND_ENCODER,
                                            C2Component::DOMAIN_VIDEO,
                                            codec == Codec::HEVC ? MIME_HEVC : MIME_AVC),
          mCodec(codec) {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noTimeStretch();
        setDerivedInstance(this);

        // The engine consumes graphic buffers by fd. CPU_READ is advertised on
        // purpose: QTI gralloc never allocates UBWC for CPU-readable buffers,
        // and the engine is configured for linear NV12, so a camera that would
        // otherwise pick UBWC for its video stream can't hand us compressed
        // frames. It also lets checkYuvLayout() map the first frame.
        addParameter(DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                             .withConstValue(new C2StreamUsageTuning::input(
                                     0u, (uint64_t)C2AndroidMemoryUsage::HW_CODEC_READ |
                                                 C2MemoryUsage::CPU_READ))
                             .build());

        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                             .withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
                             .withFields({
                                     C2F(mSize, width).inRange(96, 4096, 2),
                                     C2F(mSize, height).inRange(96, 4096, 2),
                             })
                             .withSetter(SizeSetter)
                             .build());

        addParameter(DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                             .withDefault(new C2StreamFrameRateInfo::output(0u, 30.))
                             .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                             .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                             .withDefault(new C2StreamBitrateInfo::output(0u, kDefaultBitrate))
                             .withFields({C2F(mBitrate, value).inRange(4096, 160000000)})
                             .withSetter(Setter<decltype(*mBitrate)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
                             .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
                             .withFields({C2F(mSyncFramePeriod, value).any()})
                             .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
                             .build());

        // Profile/level vocabulary differs per codec; the setter clamps to the
        // codec's own default so a wrong-codec value from a client cannot leak
        // through to venc_set_param().
        if (codec == Codec::HEVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::output(
                                    0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_MAIN_4_1))
                            .withFields({
                                    C2F(mProfileLevel, profile).oneOf({PROFILE_HEVC_MAIN}),
                                    C2F(mProfileLevel, level)
                                            .oneOf({LEVEL_HEVC_MAIN_3, LEVEL_HEVC_MAIN_3_1,
                                                    LEVEL_HEVC_MAIN_4, LEVEL_HEVC_MAIN_4_1,
                                                    LEVEL_HEVC_MAIN_5, LEVEL_HEVC_MAIN_5_1}),
                            })
                            .withSetter(HevcProfileLevelSetter, mSize, mFrameRate, mBitrate)
                            .build());
        } else {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::output(
                                    0u, PROFILE_AVC_CONSTRAINED_BASELINE, LEVEL_AVC_4_1))
                            .withFields({
                                    C2F(mProfileLevel, profile)
                                            .oneOf({PROFILE_AVC_CONSTRAINED_BASELINE,
                                                    PROFILE_AVC_BASELINE, PROFILE_AVC_MAIN,
                                                    PROFILE_AVC_HIGH}),
                                    C2F(mProfileLevel, level)
                                            .oneOf({LEVEL_AVC_3, LEVEL_AVC_3_1, LEVEL_AVC_3_2,
                                                    LEVEL_AVC_4, LEVEL_AVC_4_1, LEVEL_AVC_4_2,
                                                    LEVEL_AVC_5, LEVEL_AVC_5_1}),
                            })
                            .withSetter(ProfileLevelSetter, mSize, mFrameRate, mBitrate)
                            .build());
        }
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output> &me,
                                  const C2P<C2StreamPictureSizeInfo::input> &size,
                                  const C2P<C2StreamFrameRateInfo::output> &frameRate,
                                  const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        (void)size;
        (void)frameRate;
        (void)bitrate;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_AVC_CONSTRAINED_BASELINE;
        }
        if (!me.F(me.v.level).supportsAtAll(me.v.level)) {
            me.set().level = LEVEL_AVC_4_1;
        }
        return C2R::Ok();
    }

    static C2R HevcProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output> &me,
                                      const C2P<C2StreamPictureSizeInfo::input> &size,
                                      const C2P<C2StreamFrameRateInfo::output> &frameRate,
                                      const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        (void)size;
        (void)frameRate;
        (void)bitrate;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_HEVC_MAIN;
        }
        if (!me.F(me.v.level).supportsAtAll(me.v.level)) {
            me.set().level = LEVEL_HEVC_MAIN_4_1;
        }
        return C2R::Ok();
    }

    Codec getCodec() const { return mCodec; }
    uint32_t getWidth() const { return mSize->width; }
    uint32_t getHeight() const { return mSize->height; }
    uint32_t getBitrate() const { return mBitrate->value; }
    float getFrameRate() const { return mFrameRate->value; }
    uint32_t getSyncFramePeriod() const { return mSyncFramePeriod->value; }
    std::shared_ptr<C2StreamProfileLevelInfo::output> getProfileLevel() const {
        return mProfileLevel;
    }

private:
    const Codec mCodec;
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
};

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------

C2VencComponent::C2VencComponent(const char *name, c2_node_id_t id,
                                 const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl) {}

OMX_VIDEO_CODINGTYPE C2VencComponent::omxCoding() const {
    return mIntf->getCodec() == Codec::HEVC ? OMX_VIDEO_CodingHEVC : OMX_VIDEO_CodingAVC;
}

C2VencComponent::~C2VencComponent() {
    closeEngine();
}

bool C2VencComponent::is_secure_session(void) {
    return false;
}

c2_status_t C2VencComponent::openEngine() {
    if (mEngineOpened) {
        return C2_OK;
    }

    // venc_dev binds to the host base, not the component; the cast has to be
    // explicit because C2VencComponent has two bases.
    mDev = new (std::nothrow) venc_dev(static_cast<C2VencHost *>(this));
    if (mDev == nullptr) {
        ALOGE("failed to allocate venc_dev");
        return C2_NO_MEMORY;
    }

    // The engine takes the OMX coding enum, not a mime. OMX_VIDEO_CodingHEVC
    // selects V4L2_PIX_FMT_HEVC in venc_open(); the HEIC variant is not used.
    if (!mDev->venc_open(omxCoding())) {
        ALOGE("venc_open failed");
        delete mDev;
        mDev = nullptr;
        return C2_CORRUPTED;
    }

    mEngineOpened = true;
    const c2_status_t status = configureEngine();
    if (status != C2_OK) {
        closeEngine();
        return status;
    }

    // The engine does not spawn its own V4L2 poll loop -- the host does, and
    // passes itself as the argument. Without this nothing ever dequeues a
    // finished buffer, so the output slots fill up and every subsequent frame
    // stalls. venc_set_message_thread_id() is what lets venc_close() join it.
    // Publish the engine and its buffer arrays before the thread reads them.
    handle = mDev;
    pthread_t tid;
    int r = pthread_create(&tid, nullptr, venc_dev::async_venc_message_thread,
                           static_cast<C2VencHost *>(this));
    if (r != 0) {
        ALOGE("failed to create the engine message thread: %d", r);
        closeEngine();
        return C2_NO_MEMORY;
    }
    mDev->venc_set_message_thread_id(tid);
    return C2_OK;
}

bool C2VencComponent::allocStaging() {
    const uint32_t width = mWidth;
    const uint32_t height = mHeight;
    mStageYStride = VENUS_Y_STRIDE(COLOR_FMT_NV12, width);
    mStageYScanlines = VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
    mStageUvStride = VENUS_UV_STRIDE(COLOR_FMT_NV12, width);
    const uint32_t size = VENUS_BUFFER_SIZE(COLOR_FMT_NV12, width, height);
    mStageSize = std::max<uint32_t>(size, mInBufSize);
    if (!size || mStageSize > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    mStageIon.resize(mInBufCount);
    mStageBase.resize(mInBufCount, nullptr);
    for (auto &ion : mStageIon) {
        ion.data_fd = -1;
        ion.dev_fd = -1;
    }
    for (uint32_t slot = 0; slot < mInBufCount; ++slot) {
        if (!alloc_map_ion_memory(mStageSize, &mStageIon[slot], ION_FLAG_CACHED)) {
            releaseStaging();
            return false;
        }
        void *base = mmap(nullptr, mStageSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                          mStageIon[slot].data_fd, 0);
        if (base == MAP_FAILED) {
            ALOGE("Cannot map NV12 input slot %u: %s", slot, strerror(errno));
            releaseStaging();
            return false;
        }
        mStageBase[slot] = base;
    }
    return true;
}

void C2VencComponent::releaseStaging() {
    for (size_t slot = 0; slot < mStageIon.size(); ++slot) {
        if (mStageBase[slot]) {
            munmap(mStageBase[slot], mStageSize);
        }
        free_ion_memory(&mStageIon[slot]);
    }
    mStageIon.clear();
    mStageBase.clear();
    mStageSize = 0;
}

c2_status_t C2VencComponent::stageFrame(const C2ConstGraphicBlock &block, uint32_t slot) {
    const c2_status_t fenceStatus = block.fence().wait(500000000);
    if (fenceStatus != C2_OK) {
        return fenceStatus;
    }
    const C2GraphicView view = block.map().get();
    if (view.error() != C2_OK) {
        return view.error();
    }

    const uint32_t width = mWidth;
    const uint32_t height = mHeight;
    const C2Rect crop = view.crop();
    if (crop.width != width || crop.height != height) {
        return C2_BAD_VALUE;
    }

    const int fd = mStageIon[slot].data_fd;
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (TEMP_FAILURE_RETRY(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) < 0) {
        ALOGE("Cannot start writing NV12 slot %u: %s", slot, strerror(errno));
        return C2_CORRUPTED;
    }
    auto *y = static_cast<uint8_t *>(mStageBase[slot]);
    const C2PlanarLayout layout = view.layout();
    bool copied = false;
    if (layout.type == C2PlanarLayout::TYPE_RGB || layout.type == C2PlanarLayout::TYPE_RGBA) {
        auto *uv = y + static_cast<size_t>(mStageYStride) * mStageYScanlines;
        copied = ConvertRgbToNv12(view, width, height, y, mStageYStride, uv, mStageUvStride);
    } else if (layout.type == C2PlanarLayout::TYPE_YUV && layout.numPlanes == 3 &&
               crop.left == 0 && crop.top == 0) {
        MediaImage2 image = CreateYUV420SemiPlanarMediaImage2(width, height, mStageYStride,
                                                             mStageYScanlines);
        image.mPlane[1].mRowInc = mStageUvStride;
        image.mPlane[2].mRowInc = mStageUvStride;
        copied = ImageCopy(y, &image, view) == OK;
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (TEMP_FAILURE_RETRY(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) < 0) {
        ALOGE("Cannot synchronize NV12 slot %u: %s", slot, strerror(errno));
        return C2_CORRUPTED;
    }
    return copied ? C2_OK : C2_BAD_VALUE;
}

c2_status_t C2VencComponent::configureEngine() {
    const uint32_t width = mIntf->getWidth();
    const uint32_t height = mIntf->getHeight();
    mWidth = width;
    mHeight = height;

    // Port definition drives buffer geometry inside the engine, and
    // m_sInPortDef is one of the members it reads back out of the host.
    m_sInPortDef.format.video.nFrameWidth = width;
    m_sInPortDef.format.video.nFrameHeight = height;
    m_sInPortDef.format.video.nStride = width;
    m_sInPortDef.format.video.nSliceHeight = height;
    m_sInPortDef.format.video.xFramerate = (OMX_U32)(mIntf->getFrameRate() * 65536);
    m_sInPortDef.format.video.eColorFormat =
            (OMX_COLOR_FORMATTYPE)QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m;

    // Buffer requirements must be read BEFORE the port is configured:
    // venc_set_param(OMX_IndexParamPortDefinition) rejects the call outright if
    // nBufferCountActual is below the driver's mincount
    // (video_encoder_device_v4l2.cpp, PORT_INDEX_IN case), and a zero-filled
    // port definition trips exactly that.
    OMX_U32 inMin = 0, outMin = 0;
    if (!mDev->venc_get_buf_req(&inMin, &mInBufCount, &mInBufSize, 0 /*PORT_INDEX_IN*/)) {
        ALOGE("venc_get_buf_req(input) failed");
        return C2_CORRUPTED;
    }
    if (mInBufCount < inMin) {
        mInBufCount = inMin;
    }

    OMX_PARAM_PORTDEFINITIONTYPE portDef;
    memset(&portDef, 0, sizeof(portDef));
    portDef.nSize = sizeof(portDef);
    portDef.nPortIndex = 0;  // PORT_INDEX_IN
    portDef.eDir = OMX_DirInput;
    portDef.eDomain = OMX_PortDomainVideo;
    portDef.nBufferCountMin = inMin;
    portDef.nBufferCountActual = mInBufCount;
    portDef.nBufferSize = mInBufSize;
    portDef.format.video = m_sInPortDef.format.video;
    if (!mDev->venc_set_param(&portDef, OMX_IndexParamPortDefinition)) {
        ALOGE("venc_set_param(input port) failed (count %u min %u size %u)",
              mInBufCount, inMin, mInBufSize);
        return C2_CORRUPTED;
    }

    if (!mDev->venc_get_buf_req(&outMin, &mOutBufCount, &mOutBufSize, 1 /*PORT_INDEX_OUT*/)) {
        ALOGE("venc_get_buf_req(output) failed");
        return C2_CORRUPTED;
    }
    if (mOutBufCount < outMin) {
        mOutBufCount = outMin;
    }

    memset(&portDef, 0, sizeof(portDef));
    portDef.nSize = sizeof(portDef);
    portDef.nPortIndex = 1;  // PORT_INDEX_OUT
    portDef.eDir = OMX_DirOutput;
    portDef.eDomain = OMX_PortDomainVideo;
    portDef.nBufferCountMin = outMin;
    portDef.nBufferCountActual = mOutBufCount;
    portDef.nBufferSize = mOutBufSize;
    portDef.format.video = m_sInPortDef.format.video;
    portDef.format.video.eCompressionFormat = omxCoding();
    portDef.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    portDef.format.video.nBitrate = mIntf->getBitrate();
    if (!mDev->venc_set_param(&portDef, OMX_IndexParamPortDefinition)) {
        ALOGE("venc_set_param(output port) failed (count %u min %u size %u)",
              mOutBufCount, outMin, mOutBufSize);
        return C2_CORRUPTED;
    }

    // Profile and level, on the OUTPUT port. The engine rejects
    // V4L2_CID_MPEG_VIDC_VIDEO_NUM_P_FRAMES inside venc_start() if the session
    // profile was never established, which surfaces as the misleading
    // "Reconfiguring intra period failed".
    OMX_VIDEO_PARAM_PROFILELEVELTYPE profileLevel;
    memset(&profileLevel, 0, sizeof(profileLevel));
    profileLevel.nSize = sizeof(profileLevel);
    profileLevel.nPortIndex = 1;  // PORT_INDEX_OUT
    if (mIntf->getCodec() == Codec::HEVC) {
        profileLevel.eProfile = OMX_VIDEO_HEVCProfileMain;
        profileLevel.eLevel = OMX_VIDEO_HEVCMainTierLevel41;
    } else {
        profileLevel.eProfile = OMX_VIDEO_AVCProfileHigh;
        profileLevel.eLevel = OMX_VIDEO_AVCLevel41;
    }
    if (!mDev->venc_set_param(&profileLevel, OMX_IndexParamVideoProfileLevelCurrent)) {
        ALOGE("venc_set_param(profile/level) failed");
        return C2_CORRUPTED;
    }

    // Intra period. venc_dev defaults num_pframes to 29, but it only programs
    // the control during venc_start(); setting it explicitly here ties it to
    // the interface's sync-frame interval instead of a hardcoded 1s at 30fps.
    const float fps = mIntf->getFrameRate() > 0.f ? mIntf->getFrameRate() : 30.f;
    const uint32_t syncUs = mIntf->getSyncFramePeriod();
    uint32_t pFrames = 29;
    if (syncUs > 0 && syncUs != ~0u) {
        double frames = (double)syncUs * fps / 1000000.0;
        pFrames = frames > 1.0 ? (uint32_t)(frames - 1.0) : 0;
        // The driver rejects very long GOPs; cap it rather than have the
        // control fail outright and lose the setting entirely.
        if (pFrames > kMaxPFrames) {
            pFrames = kMaxPFrames;
        }
    }
    OMX_VIDEO_CONFIG_AVCINTRAPERIOD intraPeriod;
    memset(&intraPeriod, 0, sizeof(intraPeriod));
    intraPeriod.nSize = sizeof(intraPeriod);
    intraPeriod.nPortIndex = 1;  // PORT_INDEX_OUT
    intraPeriod.nPFrames = pFrames;
    intraPeriod.nIDRPeriod = 1;
    if (!mDev->venc_set_config(&intraPeriod, OMX_IndexConfigVideoAVCIntraPeriod)) {
        // Not fatal. The driver rejects large values (screenrecord asks for a
        // 20s keyframe interval, i.e. 599 P-frames at 30fps, and the control
        // fails), and venc_start() programs its own default of 29 regardless.
        // A suboptimal GOP is much better than refusing to encode.
        ALOGW("venc_set_config(intra period, pframes=%u) rejected; "
              "falling back to the engine default", pFrames);
    }

    OMX_VIDEO_PARAM_BITRATETYPE bitrate;
    memset(&bitrate, 0, sizeof(bitrate));
    bitrate.nSize = sizeof(bitrate);
    bitrate.nPortIndex = 1;
    bitrate.eControlRate = OMX_Video_ControlRateVariable;
    bitrate.nTargetBitrate = mIntf->getBitrate();
    if (!mDev->venc_set_param(&bitrate, OMX_IndexParamVideoBitrate)) {
        ALOGE("venc_set_param(bitrate) failed");
        return C2_CORRUPTED;
    }

    // Re-query now that both ports carry real geometry. The first query (before
    // venc_set_param) could only give the driver's defaults -- it exists to
    // learn mincount -- so the sizes from it are wrong: 98304 bytes for a
    // 1080x2340 NV12 input, where ~3.8 MB is correct.
    OMX_U32 reMin = 0;
    if (mDev->venc_get_buf_req(&reMin, &mInBufCount, &mInBufSize, 0 /*PORT_INDEX_IN*/)) {
        if (mInBufCount < reMin) {
            mInBufCount = reMin;
        }
    } else {
        ALOGW("re-query of input buffer requirements failed; keeping %u x %u",
              mInBufCount, mInBufSize);
    }
    if (mDev->venc_get_buf_req(&reMin, &mOutBufCount, &mOutBufSize, 1 /*PORT_INDEX_OUT*/)) {
        if (mOutBufCount < reMin) {
            mOutBufCount = reMin;
        }
    } else {
        ALOGW("re-query of output buffer requirements failed; keeping %u x %u",
              mOutBufCount, mOutBufSize);
    }

    // venc_empty_buf() does REQBUFS for the OUTPUT_MPLANE queue inline, but
    // venc_fill_buf() does not do it for CAPTURE_MPLANE -- in the OMX component
    // that happens during buffer allocation, which we do not use. Without it
    // the driver has no capture buffer pool and every ftb is rejected with
    // "Failed to qbuf (ftb) to driver".
    // The engine's async thread indexes m_out_mem_ptr / m_pOutput_pmem with the
    // dequeued V4L2 index, so those arrays must exist and must be the very
    // headers we submit -- it hands the resulting pointer back as clientdata.
    if (mInBufCount == 0 || mInBufCount > MAX_V4L2_BUFS ||
        mOutBufCount == 0 || mOutBufCount > MAX_V4L2_BUFS ||
        mInBufSize == 0 || mOutBufSize == 0) {
        ALOGE("Invalid encoder buffer requirements");
        return C2_CORRUPTED;
    }
    if (!allocBufferArrays(mInBufCount, mOutBufCount)) {
        return C2_NO_MEMORY;
    }
    mInputBusy.assign(mInBufCount, false);

    if (!mDev->venc_reconfig_reqbufs()) {
        ALOGE("venc_reconfig_reqbufs failed");
        return C2_CORRUPTED;
    }

    ALOGI("engine configured %ux%u, in %u x %u bytes, out %u x %u bytes", width, height,
          mInBufCount, mInBufSize, mOutBufCount, mOutBufSize);
    return C2_OK;
}

void C2VencComponent::closeEngine() {
    if (!mDev) {
        return;
    }
    bool wasStreaming;
    {
        std::lock_guard<std::mutex> lock(mLock);
        wasStreaming = mStreaming;
        mStreaming = false;
    }
    if (wasStreaming) {
        mDev->venc_stop();
    }
    if (mEngineOpened) {
        // Join the poll thread before releasing any buffers it can reference.
        mDev->venc_close();
        mEngineOpened = false;
    }

    freeBufferArrays();
    releaseStaging();
    delete mDev;
    mDev = nullptr;
    handle = nullptr;

    std::lock_guard<std::mutex> lock(mLock);
    mHadInput = false;
    mOutputsSeeded = false;
    mDraining = false;
    mDriverEos = false;
    mDeliveryPosted = false;
    mError = C2_OK;
    mPool.reset();
    mOutSlots.clear();
    mInputBusy.clear();
    mInFlight.clear();
    mReadyFrames.clear();
    mCsd.reset();
}

c2_status_t C2VencComponent::onInit() {
    return openEngine();
}

c2_status_t C2VencComponent::onStop() {
    closeEngine();
    return C2_OK;
}

void C2VencComponent::onReset() {
    closeEngine();
}

void C2VencComponent::onRelease() {
    closeEngine();
}

c2_status_t C2VencComponent::onFlush_sm() {
    // STREAMOFF and join before any slot can be reused by the next session.
    closeEngine();
    return C2_OK;
}

bool C2VencComponent::seedOutputs(const std::shared_ptr<C2BlockPool> &pool) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mPool = pool;
        mOutSlots.resize(mOutBufCount);
    }
    for (uint32_t slot = 0; slot < mOutBufCount; ++slot) {
        if (!requeueOutput(slot)) {
            ALOGE("Cannot queue initial capture slot %u", slot);
            return false;
        }
    }
    return true;
}

bool C2VencComponent::requeueOutput(uint32_t slot) {
    std::shared_ptr<C2BlockPool> pool;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mStreaming || mDriverEos || mError != C2_OK) {
            return true;
        }
        pool = mPool;
    }
    std::shared_ptr<C2LinearBlock> block;
    const C2MemoryUsage usage{C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    const c2_status_t status = pool->fetchLinearBlock(mOutBufSize, usage, &block);
    if (status != C2_OK || !block || !block->handle() || block->handle()->numFds < 1 ||
        block->size() < mOutBufSize) {
        ALOGE("Cannot allocate capture slot %u: %d", slot, status);
        return false;
    }

    std::lock_guard<std::mutex> lock(mLock);
    if (!mStreaming || mDriverEos || mError != C2_OK) {
        return true;
    }
    if (slot >= mOutSlots.size() || mOutSlots[slot]) {
        ALOGE("Capture slot %u is unavailable", slot);
        return false;
    }
    OMX_BUFFERHEADERTYPE *header = &m_out_mem_ptr[slot];
    *header = {};
    header->nSize = sizeof(*header);
    header->nAllocLen = block->size();
    header->nOffset = block->offset();
    const int fd = block->handle()->data[0];
    m_pOutput_pmem[slot].fd = fd;
    m_pOutput_pmem[slot].size = block->size();
    m_pOutput_pmem[slot].offset = block->offset();
    mOutSlots[slot] = std::move(block);
    if (!mDev->venc_fill_buf(header, nullptr, slot, fd)) {
        mOutSlots[slot].reset();
        ALOGE("Cannot submit capture slot %u", slot);
        return false;
    }
    return true;
}

void C2VencComponent::scheduleDelivery() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mStreaming || mDraining || mDeliveryPosted) {
            return;
        }
        mDeliveryPosted = true;
    }
    // This queues a marker on the same looper as process(). A driver callback
    // may already have arrived while QBUF is returning. The marker runs after
    // SimpleC2Component has placed that input work in its pending list.
    SimpleC2Component::drain_nb(static_cast<drain_mode_t>(NO_DRAIN));
}

void C2VencComponent::reportError(c2_status_t error) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (mError == C2_OK) {
            mError = error;
        }
    }
    mCv.notify_all();
    scheduleDelivery();
}

void C2VencComponent::deliverFrames(C2Work *current, bool markLastEos) {
    std::deque<EncodedFrame> ready;
    std::multimap<int64_t, uint64_t> failed;
    c2_status_t error;
    {
        std::lock_guard<std::mutex> lock(mLock);
        mDeliveryPosted = false;
        ready.swap(mReadyFrames);
        error = mError;
        if (error != C2_OK) {
            failed.swap(mInFlight);
        }
    }
    for (auto &frame : ready) {
        const bool eos = markLastEos && error == C2_OK && &frame == &ready.back();
        auto fill = [&](C2Work &work) {
            auto &out = work.worklets.front()->output;
            if (frame.config) {
                out.configUpdate.push_back(std::move(frame.config));
            }
            out.flags = eos ? C2FrameData::FLAG_END_OF_STREAM : C2FrameData::flags_t{};
            out.ordinal = work.input.ordinal;
            out.buffers = {std::move(frame.buffer)};
            work.workletsProcessed = 1u;
        };
        if (current && current->input.ordinal.frameIndex.peeku() == frame.index) {
            fill(*current);
        } else {
            finish(frame.index, [&](const std::unique_ptr<C2Work> &work) { fill(*work); });
        }
    }
    for (const auto &[timestamp, index] : failed) {
        (void)timestamp;
        auto fail = [error](C2Work &work) {
            work.result = error;
            work.workletsProcessed = 1u;
        };
        if (current && current->input.ordinal.frameIndex.peeku() == index) {
            fail(*current);
        } else {
            finish(index, [&](const std::unique_ptr<C2Work> &work) { fail(*work); });
        }
    }
}

c2_status_t C2VencComponent::drainEncoder() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mHadInput || mDriverEos || mError != C2_OK) {
            return mError;
        }
        mDraining = true;
    }
    if (!mDev->venc_handle_empty_eos_buffer()) {
        reportError(C2_CORRUPTED);
    }

    std::unique_lock<std::mutex> lock(mLock);
    if (!mCv.wait_for(lock, kEosTimeout, [this] { return mDriverEos || mError != C2_OK; })) {
        ALOGE("Timed out waiting for encoder EOS");
        mError = C2_TIMED_OUT;
    } else if (mDriverEos && !mInFlight.empty() && mError == C2_OK) {
        ALOGE("Encoder returned EOS with %zu frames outstanding", mInFlight.size());
        mError = C2_CORRUPTED;
    }
    mDraining = false;
    return mError;
}

void C2VencComponent::process(const std::unique_ptr<C2Work> &work,
                             const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    if (work->worklets.size() != 1 || !work->worklets.front()) {
        work->result = C2_BAD_VALUE;
        return;
    }
    auto &out = work->worklets.front()->output;
    out.flags = C2FrameData::flags_t{};
    out.ordinal = work->input.ordinal;
    out.buffers.clear();
    out.configUpdate.clear();

    const bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (work->input.buffers.empty()) {
        if (eos) {
            work->result = drainEncoder();
            deliverFrames();
            if (work->result == C2_OK) {
                out.flags = C2FrameData::FLAG_END_OF_STREAM;
            }
        }
        return;
    }

    bool restart;
    {
        std::lock_guard<std::mutex> lock(mLock);
        restart = mDriverEos;
        work->result = mError;
    }
    if (work->result != C2_OK) {
        return;
    }
    if (restart) {
        closeEngine();
    }
    const auto &buffer = work->input.buffers.front();
    if (!buffer || buffer->data().type() != C2BufferData::GRAPHIC ||
        buffer->data().graphicBlocks().size() != 1) {
        work->result = C2_BAD_VALUE;
        return;
    }
    const C2ConstGraphicBlock block = buffer->data().graphicBlocks().front();
    work->result = openEngine();
    if (work->result != C2_OK) {
        return;
    }
    if (mStageIon.empty() && !allocStaging()) {
        work->result = C2_NO_MEMORY;
        return;
    }
    if (!mOutputsSeeded) {
        if (mDev->venc_start() != 0) {
            work->result = C2_CORRUPTED;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mLock);
            mStreaming = true;
        }
        if (!seedOutputs(pool)) {
            work->result = C2_NO_MEMORY;
            reportError(work->result);
            return;
        }
        mOutputsSeeded = true;
    }

    uint32_t slot;
    {
        std::unique_lock<std::mutex> lock(mLock);
        if (!mCv.wait_for(lock, kInputSlotTimeout, [this] {
                return mError != C2_OK ||
                        std::find(mInputBusy.begin(), mInputBusy.end(), false) != mInputBusy.end();
            })) {
            work->result = C2_TIMED_OUT;
            lock.unlock();
            reportError(work->result);
            return;
        }
        if (mError != C2_OK) {
            work->result = mError;
            return;
        }
        slot = std::find(mInputBusy.begin(), mInputBusy.end(), false) - mInputBusy.begin();
        mInputBusy[slot] = true;
    }

    work->result = stageFrame(block, slot);
    if (work->result != C2_OK) {
        ALOGE("Cannot stage input slot %u: %d", slot, work->result);
        onInputDone(&m_inp_mem_ptr[slot]);
        reportError(work->result);
        return;
    }

    const int64_t timestamp = work->input.ordinal.timestamp.peekll();
    const uint64_t index = work->input.ordinal.frameIndex.peeku();
    OMX_BUFFERHEADERTYPE *header = &m_inp_mem_ptr[slot];
    *header = {};
    header->nSize = sizeof(*header);
    header->nTimeStamp = timestamp;
    header->nFilledLen = mStageSize;
    header->nAllocLen = mStageSize;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (mError != C2_OK) {
            mInputBusy[slot] = false;
            work->result = mError;
            return;
        }
        mInFlight.emplace(timestamp, index);
    }
    if (!mDev->venc_empty_buf(header, nullptr, slot, mStageIon[slot].data_fd)) {
        {
            std::lock_guard<std::mutex> lock(mLock);
            const auto range = mInFlight.equal_range(timestamp);
            const auto entry = std::find_if(range.first, range.second, [index](const auto &value) {
                return value.second == index;
            });
            if (entry != range.second) {
                mInFlight.erase(entry);
            }
        }
        onInputDone(header);
        work->result = C2_CORRUPTED;
        reportError(work->result);
        return;
    }
    mHadInput = true;

    work->workletsProcessed = 0u;
    if (eos) {
        work->result = drainEncoder();
        deliverFrames(work.get());
        if (work->result == C2_OK && work->workletsProcessed) {
            out.flags = C2FrameData::FLAG_END_OF_STREAM;
        } else {
            if (work->result == C2_OK) {
                work->result = C2_CORRUPTED;
            }
            out.buffers.clear();
            out.configUpdate.clear();
            work->workletsProcessed = 1u;
        }
    }
}

c2_status_t C2VencComponent::drain(uint32_t drainMode,
                                  const std::shared_ptr<C2BlockPool> &pool) {
    (void)pool;
    if (drainMode == NO_DRAIN) {
        deliverFrames();
        return C2_OK;
    }
    if (drainMode != DRAIN_COMPONENT_WITH_EOS) {
        return C2_OMITTED;
    }
    const c2_status_t status = drainEncoder();
    deliverFrames(nullptr, true);
    return status;
}

void C2VencComponent::onInputDone(OMX_BUFFERHEADERTYPE *header) {
    if (!m_inp_mem_ptr || !header) {
        return;
    }
    const size_t slot = header - m_inp_mem_ptr;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (slot >= mInputBusy.size()) {
            return;
        }
        mInputBusy[slot] = false;
    }
    mCv.notify_all();
}

void C2VencComponent::onOutputDone(OMX_BUFFERHEADERTYPE *header, const ::venc_msg &msg) {
    if (!m_out_mem_ptr || !header) {
        return;
    }
    const size_t slot = header - m_out_mem_ptr;
    std::shared_ptr<C2LinearBlock> block;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mStreaming || mError != C2_OK || mDriverEos || slot >= mOutSlots.size()) {
            return;
        }
        block = std::move(mOutSlots[slot]);
    }
    const size_t length = msg.buf.len;
    const size_t offset = msg.buf.offset;
    const bool eos = (msg.buf.flags & OMX_BUFFERFLAG_EOS) != 0;
    const bool csd = (msg.buf.flags & OMX_BUFFERFLAG_CODECCONFIG) != 0;
    if (!block || offset > block->size() || length > block->size() - offset) {
        ALOGE("Invalid capture buffer range at slot %zu", slot);
        reportError(C2_CORRUPTED);
        return;
    }

    bool needsDelivery = false;
    if (length && csd) {
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            reportError(view.error());
            return;
        }
        auto config = C2StreamInitDataInfo::output::AllocUnique(length, 0u);
        if (!config) {
            reportError(C2_NO_MEMORY);
            return;
        }
        memcpy(config->m.value, view.data() + offset, length);
        std::lock_guard<std::mutex> lock(mLock);
        mCsd = std::move(config);
    } else if (length) {
        auto buffer = createLinearBuffer(block, offset, length);
        if (msg.buf.flags & OMX_BUFFERFLAG_SYNCFRAME) {
            buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
                    0u, C2Config::SYNC_FRAME));
        }
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (!mStreaming || mError != C2_OK) {
                return;
            }
            const auto pending = mInFlight.find(msg.buf.timestamp);
            if (pending != mInFlight.end()) {
                mReadyFrames.push_back({pending->second, std::move(buffer), std::move(mCsd)});
                mInFlight.erase(pending);
                needsDelivery = true;
            }
        }
        if (!needsDelivery) {
            ALOGE("Unmatched encoder timestamp %" PRId64, static_cast<int64_t>(msg.buf.timestamp));
            reportError(C2_CORRUPTED);
            return;
        }
    }

    if (eos) {
        {
            std::lock_guard<std::mutex> lock(mLock);
            mDriverEos = true;
        }
        mCv.notify_all();
    }
    if (needsDelivery) {
        scheduleDelivery();
    }
    if (!eos && !requeueOutput(slot)) {
        reportError(C2_CORRUPTED);
    }
}

int C2VencComponent::async_message_process(void *context, void *message) {
    (void)context;
    if (!message) {
        return -1;
    }
    const auto &msg = *static_cast<const ::venc_msg *>(message);
    if (msg.statuscode != VEN_S_SUCCESS) {
        ALOGE("Encoder error %lu in message %lu", msg.statuscode, msg.msgcode);
        reportError(C2_CORRUPTED);
        return -1;
    }
    if (msg.msgcode == VEN_MSG_OUTPUT_BUFFER_DONE && msg.buf.clientdata) {
        onOutputDone(static_cast<OMX_BUFFERHEADERTYPE *>(msg.buf.clientdata), msg);
    } else if (msg.msgcode == VEN_MSG_INPUT_BUFFER_DONE) {
        onInputDone(static_cast<OMX_BUFFERHEADERTYPE *>(msg.buf.clientdata));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Factory
//
// IntfImpl is a complete type only in this translation unit, so C2VencStore
// cannot build the interface itself -- this is the store's only way in.
// Shape follows C2SoftAvcEncFactory, except the reflector is injected rather
// than taken from GetCodec2PlatformComponentStore(): the store owns the
// reflector the HAL reflects params through, and the platform store is the
// wrong one inside a vendor HAL process.
// ---------------------------------------------------------------------------

class C2VencComponentFactory : public C2ComponentFactory {
public:
    C2VencComponentFactory(std::shared_ptr<C2ReflectorHelper> helper, C2VencComponent::Codec codec)
        : mHelper(std::move(helper)), mCodec(codec) {}
    ~C2VencComponentFactory() override = default;

    c2_status_t createComponent(c2_node_id_t id,
                                std::shared_ptr<C2Component> *const component,
                                ComponentDeleter deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2VencComponent(name(), id,
                                    std::make_shared<C2VencComponent::IntfImpl>(mHelper, mCodec)),
                deleter);
        return C2_OK;
    }

    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface> *const interface,
                                InterfaceDeleter deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2VencComponent::IntfImpl>(
                        name(), id,
                        std::make_shared<C2VencComponent::IntfImpl>(mHelper, mCodec)),
                deleter);
        return C2_OK;
    }

private:
    const char *name() const {
        return mCodec == C2VencComponent::Codec::HEVC ? COMPONENT_NAME_HEVC : COMPONENT_NAME_AVC;
    }

    std::shared_ptr<C2ReflectorHelper> mHelper;
    const C2VencComponent::Codec mCodec;
};

std::unique_ptr<C2ComponentFactory> CreateC2VencFactory(
        const std::string &name, std::shared_ptr<C2ReflectorHelper> helper) {
    if (name == COMPONENT_NAME_AVC) {
        return std::make_unique<C2VencComponentFactory>(std::move(helper),
                                                        C2VencComponent::Codec::AVC);
    }
    if (name == COMPONENT_NAME_HEVC) {
        return std::make_unique<C2VencComponentFactory>(std::move(helper),
                                                        C2VencComponent::Codec::HEVC);
    }
    return nullptr;
}

}  // namespace android
