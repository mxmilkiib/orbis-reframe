#include "ProbeClip.h"

#include <InsvDemux.h> // insv::InsvDemux

namespace stitch {

bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err)
{
    insv::InsvDemux demux;
    if (!demux.open(clip_path, err))
        return false;
    if (demux.lensCount() < 1) {
        err = "no video streams in " + clip_path;
        return false;
    }
    const insv::InsvStreamInfo& l0 = demux.lens(0);
    out.width = l0.width;
    out.height = l0.height;
    out.fps = l0.fps;
    out.duration_ms = l0.durationMs;
    out.bitDepth = l0.bitDepth;
    return true;
}

} // namespace stitch
