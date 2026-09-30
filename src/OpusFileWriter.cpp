#include "OpusFileWriter.h"

#include "Util.h"

#include <opus/opusenc.h>

namespace {

int WritePage(void* user, const unsigned char* data, opus_int32 length) {
    auto* file = static_cast<FILE*>(user);
    if (std::fwrite(data, 1, static_cast<size_t>(length), file) != static_cast<size_t>(length)) return 1;
    return std::fflush(file) == 0 ? 0 : 1;
}

int CloseFile(void* user) {
    return std::fclose(static_cast<FILE*>(user)) == 0 ? 0 : 1;
}

const OpusEncCallbacks kCallbacks = {WritePage, CloseFile};

}  // namespace

OpusFileWriter::~OpusFileWriter() {
    Close();
}

bool OpusFileWriter::Open(const std::wstring& path, const std::wstring& title, int bitrate,
                          std::wstring& error) {
    // libopusenc's own file helper uses narrow fopen, which mangles non-ASCII
    // Windows paths, so the file is opened here and handed over via callbacks.
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        error = L"Couldn't create " + path;
        return false;
    }

    comments_ = ope_comments_create();
    ope_comments_add(comments_, "TITLE", ToUtf8(title).c_str());
    ope_comments_add(comments_, "ENCODER", "CallRecorder " CALLRECORDER_VERSION);

    int status = OPE_OK;
    encoder_ = ope_encoder_create_callbacks(&kCallbacks, file, comments_, kSampleRate, 1, 0, &status);
    if (!encoder_) {
        std::fclose(file);
        ope_comments_destroy(comments_);
        comments_ = nullptr;
        error = L"Opus encoder error: " + FromUtf8(ope_strerror(status));
        return false;
    }

    ope_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    ope_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // By default libopusenc holds ~3 s in memory (2 s lookahead + 1 s per
    // page), all of which a crash would lose. Keep that under half a second.
    ope_encoder_ctl(encoder_, OPE_SET_DECISION_DELAY(kSampleRate / 10));
    ope_encoder_ctl(encoder_, OPE_SET_MUXING_DELAY(kSampleRate / 4));
    ope_encoder_flush_header(encoder_);
    return true;
}

bool OpusFileWriter::Write(const float* samples, int count) {
    return encoder_ && ope_encoder_write_float(encoder_, samples, count) == OPE_OK;
}

void OpusFileWriter::Close() {
    if (encoder_) {
        // libopusenc closes the file via CloseFile exactly once: at end of
        // stream during drain, or in destroy if the drain failed.
        ope_encoder_drain(encoder_);
        ope_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    if (comments_) {
        ope_comments_destroy(comments_);
        comments_ = nullptr;
    }
}
