#pragma once

#include <cstdio>
#include <string>

struct OggOpusEnc;
struct OggOpusComments;

// Writes float PCM (mono, or interleaved stereo) to an Ogg Opus file. Pages are flushed to disk as they
// are produced, so a crash or power cut leaves a playable file that is missing
// well under a second (plus the recorder's 100 ms mixing delay).
class OpusFileWriter {
public:
    static constexpr int kSampleRate = 48000;

    OpusFileWriter() = default;
    ~OpusFileWriter();
    OpusFileWriter(const OpusFileWriter&) = delete;
    OpusFileWriter& operator=(const OpusFileWriter&) = delete;

    // channels is 1 or 2. Fails, without touching it, if the file already exists.
    bool Open(const std::wstring& path, const std::wstring& title, int bitrate, int channels, std::wstring& error);
    // `frames` is per channel; `samples` holds frames * channels interleaved values.
    bool Write(const float* samples, int frames);
    void Close();

private:
    OggOpusEnc* encoder_ = nullptr;
    OggOpusComments* comments_ = nullptr;
};
