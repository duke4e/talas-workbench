#pragma once

// ADR-0030: what a plugin's host says about its musical transport, for the
// render that starts at the moment it describes.
namespace talas::runtime
{
struct HostMusicalTransport
{
    // Quarter notes a minute, positive and finite.
    double tempoBpm = 120.0;
    // The song position at the render's first frame, in quarter notes;
    // negative in a count-in before the song's start.
    double quarterPosition = 0.0;
    bool playing = false;
    // The position jumped since the previous render: a locate, a loop's
    // return, a start somewhere else than where the host stopped.
    bool relocated = false;
    // The host's play, stop and jumps also start, stop and move the Sound
    // and the Timeline it plays, from the musical position the host gives.
    // Otherwise the Sound runs on its own and only the transport's Event
    // Values follow the host.
    bool drivesSound = false;
};
}
