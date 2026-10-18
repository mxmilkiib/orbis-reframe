#pragma once

#include <QtGlobal>

#include <cstring>

// A transition at a cut on the main track: instead of switching at the cut,
// the picture goes over from the clip before it to the clip after it over
// `durationMs`, centred on the cut, and their own sound with it.
//
// A crossfade blends the two. It uses footage trimmed off the clips
// ("handles"): the clip before plays on past its trim-out for half the
// duration, the clip after starts half the duration before its trim-in. So
// the clips stay end to end and the timeline keeps its length; where there
// isn't that much footage past the trims the transition is shorter
// (Timeline::transitionSpan).
//
// A dip to black fades the clip before out to black up to the cut, and the
// clip after in from black after it, sound and all. Each clip fades within
// what's kept of it, so it needs no footage past the trims.
//
// A transition belongs to the cut between two clips (by their ids), not to
// either clip: reordering them or removing one drops it; splitting or
// duplicating the clip before it keeps it at the same cut.
struct Transition {
    enum class Type { Crossfade, DipToBlack };

    static constexpr qint64 kDefaultMs = 1000;
    static constexpr qint64 kMinMs = 100;
    static constexpr qint64 kMaxMs = 10000;
    // The mime type of a transition dragged from the Transitions tab; the
    // data is the type's name (typeName).
    static constexpr const char *kMimeType = "application/x-orbis-transition";

    int leftClipId = 0;  // the clip before the cut
    int rightClipId = 0; // ... and after it
    Type type = Type::Crossfade;
    qint64 durationMs = kDefaultMs; // as set; what plays may be shorter

    bool operator==(const Transition &o) const
    {
        return leftClipId == o.leftClipId && rightClipId == o.rightClipId && type == o.type &&
               durationMs == o.durationMs;
    }

    // Plays footage past the two clips' trims (a crossfade does).
    static bool usesHandles(Type t) { return t == Type::Crossfade; }
    bool usesHandles() const { return usesHandles(type); }

    // The project file's / a drag's name for a type, and back (an unknown
    // name, from a newer build: a crossfade).
    static const char *typeName(Type t)
    {
        switch (t) {
        case Type::Crossfade:
            break;
        case Type::DipToBlack:
            return "dipToBlack";
        }
        return "crossfade";
    }
    static Type typeFromName(const char *name)
    {
        return name && std::strcmp(name, "dipToBlack") == 0 ? Type::DipToBlack : Type::Crossfade;
    }
};
