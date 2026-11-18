/*
 * Codec2 video encoder for sm8150, driving the in-tree venc_dev V4L2 engine.
 *
 * The component is both a SimpleC2Component (so it inherits the work queue,
 * threading and start/stop/flush plumbing from AOSP) and a C2VencHost (so
 * venc_dev can call back into it for ION and completions). The engine itself
 * is the shipped sm8150 one, compiled with -DVENC_HOST_C2; nothing in its
 * V4L2 handling is reimplemented here.
 *
 * Buffers cross the boundary as OMX_BUFFERHEADERTYPE because that is what
 * venc_dev's signatures take. They are plain structs -- this fabricates them
 * around C2 blocks rather than owning any OMX machinery.
 */

#ifndef C2_VENC_COMPONENT_H_
#define C2_VENC_COMPONENT_H_

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>

#include "C2VencHost.h"

class venc_dev;
struct venc_msg;

namespace android {

struct C2VencComponent : public SimpleC2Component, public C2VencHost {
    class IntfImpl;

    // One component class, two codecs. venc_dev already handles both; the
    // only differences up here are the name, mime, profile/level vocabulary
    // and the OMX coding enum passed to venc_open().
    enum class Codec { AVC, HEVC };

    C2VencComponent(const char *name, c2_node_id_t id,
                    const std::shared_ptr<IntfImpl> &intfImpl);
    ~C2VencComponent() override;

    // SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
                 const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode,
                      const std::shared_ptr<C2BlockPool> &pool) override;

    // C2VencHost -- venc_dev's async message thread lands here.
    int async_message_process(void *context, void *message) override;
    bool is_secure_session(void) override;

private:
    struct EncodedFrame {
        uint64_t index;
        std::shared_ptr<C2Buffer> buffer;
        std::unique_ptr<C2StreamInitDataInfo::output> config;
    };

    // msm_vidc requires the full capture queue before it can encode input.
    bool seedOutputs(const std::shared_ptr<C2BlockPool> &pool);
    bool requeueOutput(uint32_t slot);
    void onOutputDone(OMX_BUFFERHEADERTYPE *hdr, const ::venc_msg &msg);
    void onInputDone(OMX_BUFFERHEADERTYPE *hdr);
    c2_status_t drainEncoder();
    void reportError(c2_status_t error);
    void scheduleDelivery();
    void deliverFrames(C2Work *current = nullptr, bool markLastEos = false);

    OMX_VIDEO_CODINGTYPE omxCoding() const;
    c2_status_t openEngine();
    void closeEngine();
    c2_status_t configureEngine();

    std::shared_ptr<IntfImpl> mIntf;
    venc_dev *mDev = nullptr;
    bool mEngineOpened = false;
    bool mHadInput = false;
    bool mOutputsSeeded = false;

    // One mapped NV12 allocation per driver input slot. This also keeps the
    // source gralloc handle out of the V4L2 queue after process() returns.
    bool allocStaging();
    void releaseStaging();
    c2_status_t stageFrame(const C2ConstGraphicBlock &block, uint32_t slot);
    std::vector<venc_ion> mStageIon;
    std::vector<void *> mStageBase;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mStageSize = 0;
    uint32_t mStageYStride = 0;
    uint32_t mStageYScanlines = 0;
    uint32_t mStageUvStride = 0;

    // Shared by the Codec2 looper and the encoder's dequeue thread.
    std::mutex mLock;
    std::condition_variable mCv;
    bool mStreaming = false;
    bool mDraining = false;
    bool mDriverEos = false;
    bool mDeliveryPosted = false;
    c2_status_t mError = C2_OK;
    std::shared_ptr<C2BlockPool> mPool;
    std::vector<std::shared_ptr<C2LinearBlock>> mOutSlots;
    std::vector<bool> mInputBusy;
    // Keep equal timestamps in submission order.
    std::multimap<int64_t, uint64_t> mInFlight;
    std::deque<EncodedFrame> mReadyFrames;
    std::unique_ptr<C2StreamInitDataInfo::output> mCsd;

    OMX_U32 mInBufCount = 0;
    OMX_U32 mInBufSize = 0;
    OMX_U32 mOutBufCount = 0;
    OMX_U32 mOutBufSize = 0;

    C2_DO_NOT_COPY(C2VencComponent);
};

}  // namespace android

#endif  // C2_VENC_COMPONENT_H_
