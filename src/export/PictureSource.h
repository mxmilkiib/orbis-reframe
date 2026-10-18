#pragma once

#include "FlatVideoReader.h" // VideoFrame

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Where a layer's pictures come from when they aren't a flat video file's
// frames: a 360 clip's view, rendered through its stitch -- the other clip
// of a transition, drawn over the main picture. The layers (the export's
// InsetCompositor, playback's InsetFeed) read it the way they read a
// FlatVideoReader: frameAt() for the picture at a source time, readNext()
// to run on. RGBA frames, opaque.
//
// Made by a PictureSource::Factory on the thread that reads it (opening a
// stitch is slow: it happens there, in open()). Not thread-safe.

namespace exportpipe {

class PictureSource {
public:
    using Factory = std::function<std::unique_ptr<PictureSource>()>;

    virtual ~PictureSource() = default;

    // False (with `err`) if it can't be read.
    virtual bool open(std::string &err) = 0;
    // The source's frame rate (valid once open).
    virtual double fps() const = 0;
    // Size of the frames handed out from the next one on.
    virtual void setOutputSize(int width, int height) = 0;
    // The frame showing at source time `ms`: reads forward from the current
    // position, and jumps only when `ms` is behind it or well ahead. The
    // pointer stays valid until the next call. Null at the end or on error
    // (lastError()).
    virtual const VideoFrame *frameAt(int64_t ms) = 0;
    // The next frame. False at the end or on error.
    virtual bool readNext(VideoFrame &out) = 0;
    virtual const std::string &lastError() const = 0;
};

} // namespace exportpipe
