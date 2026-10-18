#pragma once

#include <QString>

class QWidget;
class EquirectViewportWindow;

// What this computer can't do, said once and plainly: checks for the things a
// missing package or an unsupported GPU takes away, and the texts that explain
// them -- shared by the startup warning, the export pre-check, Help > About and
// the panels.
namespace syscheck {

// A hardware video encoder the export can use (FFmpeg's NVENC, Vulkan-video
// or VAAPI wrapper; exportpipe::hwEncoderAvailable).
bool hwEncoderAvailable();
QString hwEncoderMissingText();

// Whether the viewport's GPU has Vulkan video decode for H.265 (the X5's
// codec in every mode). Without it clips decode on the CPU: slow for 5.7K / 8K.
// False too if the viewport hasn't started Vulkan yet.
bool gpuVideoDecode(EquirectViewportWindow *viewport);
QString gpuDecodeMissingText();

// Empty when Track subject / the plate scan can run; else why not (not in
// this build, or the model file is missing).
QString trackUnavailableReason();
QString plateScanUnavailableReason();

// Once the viewport has started Vulkan: warns about a missing GPU video
// decode or NVENC in one dialog, with "Don't show this again" (per problem,
// stored in ~/.config/orbis-reframe/orbis-reframe.conf).
void warnAtStartup(QWidget *parent, EquirectViewportWindow *viewport);

} // namespace syscheck
