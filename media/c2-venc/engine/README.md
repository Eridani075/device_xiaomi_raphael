# Vendored sm8250 V4L2 encoder engine

The SM8150 encoder snapshot documented in [../README.md](../README.md)
contains this copy of the sm8250 media tree's `mm-video-v4l2/vidc/venc`
and `mm-video-v4l2/vidc/common` code. Its V4L2 controls match the 5.4 VIDC
interface used on Raphael. The files retain their Linux Foundation
BSD 3-Clause license headers.

The engine builds in the Raphael Soong namespace with qualified SM8150
module dependencies. It uses the sm8250 include paths and the generated
5.4 kernel headers. The sm8250 audio modules stay out of Make.

Preserve the `VENC_HOST_C2` host selection and thread teardown when
updating the engine. The Codec2 host joins the engine poll thread before
releasing its buffers. The dequeue path checks both queues' buffer indexes
and reports driver failures to the component.
