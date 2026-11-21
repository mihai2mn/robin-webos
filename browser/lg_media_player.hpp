// SPDX-License-Identifier: GPL-3.0-or-later
// LG's buffer-feed media pipeline (StarfishMediaAPIs in libplayerAPIs, the NDK's starfish-media-pipeline header),
// driven the way Chromium-based browsers on webOS drive it: a BUFFERSTREAM Load whose option.windowId is the
// exported video window made on the surface that draws the page (engine/webos-backend), elementary-stream access
// units fed with their media timestamps, and the pipeline's own Play, Pause and Seek. The page's hole and the video
// window are placed by the web process; nothing here touches the screen.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class StarfishMediaAPIs;
struct WpebMediaHdr;

namespace browser {

class LgMediaPlayer {
public:
    enum class Codec { H264, Hevc, Vp9, Av1 };
    enum class Feed { Ok, Full, Error };

    LgMediaPlayer() = default;
    ~LgMediaPlayer();  // unloads
    LgMediaPlayer(const LgMediaPlayer &) = delete;
    LgMediaPlayer &operator=(const LgMediaPlayer &) = delete;

    static bool available();  // false in host builds, and on firmware without a usable Feed
    static std::string unavailable_reason();

    // The playback's audio, when it goes to the same pipeline: LG then syncs the video to it. AAC frames carry ADTS
    // headers; Opus (experimental) comes as packets, with its OpusHead declared at Load (opusInfo).
    struct Audio {
        enum class Codec { Aac, Opus, Pcm } codec = Codec::Aac;  // Pcm: S16LE interleaved stereo the web process decoded
        int rate = 0, channels = 0;
        int pcm_lead_ms = 0;  // Pcm: the audio is stamped this much later (LG's PCM path plays it earlier than its video; see the kit's pcm_lead_ms_for)
        std::vector<uint8_t> header;
    };
    bool load(const std::string &app_id, const std::string &window_id, Codec codec, int width, int height, double fps,
              const Audio *audio, std::string *error);
    // One access unit (Annex B for H.264/HEVC) or AAC frame, presentation time in microseconds of media time.
    Feed feed(bool audio, const uint8_t *data, size_t size, int64_t pts_us);
    bool has_audio() const { return has_audio_; }
    // The pipeline's last error code (0: none since the Load) and the frames it has presented: a stream that failed before its first
    // picture (decoder not yet free, resource contention) can be loaded again.
    int error() const { return error_.load(); }
    int64_t frames_presented() const { return frames_.load(); }
    // HDR10/HLG metadata from the container (setHdrInfo after Load): without it LG's panel stays in SDR for VP9/AV1
    // PQ (the kit's A/B on this TV). Nothing is declared for SDR or without mastering/light-level values.
    void set_hdr(const WpebMediaHdr &hdr);
    // The end of the stream (pushEOS): LG's pipeline plays out what it has and reports ENDOFSTREAM.
    void end_of_stream();
    // The element's volume and mute (best effort: LG's browser does not set them on this pipeline at all).
    void set_volume(unsigned permille, bool muted);
    // A seek: LG's pipeline drops what it holds (flush), and the next access unit fed starts the new position (LG's
    // Seek at its time), as Chromium on webOS does.
    void flush();
    void pause(bool paused);
    void unload();
    bool loaded() const { return api_ != nullptr; }
    int64_t fed() const { return fed_; }

    // The pipeline's event callback (any thread).
    void on_event(int type, int64_t num, const char *str);

private:
    StarfishMediaAPIs *api_ = nullptr;
    std::atomic<bool> load_completed_{false};
    bool playing_ = false, paused_ = false, seek_pending_ = true;
    bool has_audio_ = false;
    int64_t audio_lead_us_ = 0;  // added to the pts of PCM audio (Audio::pcm_lead_ms)
    int64_t fed_ = 0, full_ = 0, last_pts_us_ = 0, audio_fed_ = 0;
    std::atomic<int> error_{0};
    std::atomic<int64_t> frames_{0};  // from the pipeline's thread
    std::atomic<int64_t> dropped_{0};  // frames the pipeline dropped (event 0x30), from the pipeline's thread
    int64_t stats_at_ms_ = 0;
    int trace_units_ = 0;  // video units still to describe in the log (the first ones after a load or flush)
};

}  // namespace browser
