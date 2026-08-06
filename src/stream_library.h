// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Stream-Library (device-side storage for custom radio streams)
//
// The "Stream" sidebar tab lets users save custom direct-stream URLs as
// drag-and-drop source tiles, mirroring the Spotify-Library. Until v0.8.5
// these tiles lived in browser localStorage, so they were per-browser and
// lost on a USB-erase. They now live on the device (NVS), so they are
// cross-browser, survive re-provisioning, and can be exported/imported.
//
// streamUrl is the primary key — adding an existing streamUrl upserts.
// NOT gated on Spotify; available on every build.
#ifndef SIXBACK_STREAM_LIBRARY_H
#define SIXBACK_STREAM_LIBRARY_H

#include <Arduino.h>
#include <vector>

namespace sixback {
namespace streams {

struct StreamItem {
    String name;         // user display name
    String streamUrl;    // primary key — the (possibly redirected) stream URL
    String imageUrl;     // optional logo URL
    String icyName;      // auto-detected ICY metadata name (optional)
    String contentType;  // auto-detected content type (optional)
    String bitrate;      // auto-detected bitrate, kbps as string (optional)
};

// Load persisted streams from NVS. Call once in setup().
void init();

// Upsert by streamUrl. Persists to NVS. Returns true if a new entry was
// created, false if an existing one was updated. If `persisted` is non-null it
// receives the NVS-save result — false means the change lives in RAM only and
// will not survive a reboot (typically: partition out of space).
bool addStreamItem(const StreamItem& item, bool* persisted = nullptr);

// Remove by streamUrl. Returns true if something was removed. `persisted` as
// in addStreamItem — false after a removal means the stream will REAPPEAR on
// the next reboot (the on-flash state still has it).
bool removeStreamItem(const String& streamUrl, bool* persisted = nullptr);

// All stored streams (for the sidebar tile-grid + export).
std::vector<StreamItem> listStreams();

}  // namespace streams
}  // namespace sixback

#endif  // SIXBACK_STREAM_LIBRARY_H
