// Output routing (macOS only; the implementation is audio_route_mac.m): follow the system's default output
// device and the built-in device's data source to tell headphones from speakers, and switch Wide stereo and
// Binaural while that is allowed. iOS has none of the AudioObject hardware queries involved, hence the _mac
// name that keeps the file out of iOS builds.
#pragma once
void dm_audio_route_init(void);        // register the listeners and make one first verdict (idempotent)
