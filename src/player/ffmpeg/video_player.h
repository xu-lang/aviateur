#pragma once

#include <common/any_callable.h>

#include <memory>
#include <fstream>
#include <queue>
#include <thread>

#include "../video_player.h"
#include "../yuv_renderer.h"
#include "ffmpeg_decoder.h"
#include "gif_encoder.h"
#include "mp4_encoder.h"

struct SDL_AudioStream;

class VideoPlayerFfmpeg final : public VideoPlayer {
public:
    VideoPlayerFfmpeg(std::shared_ptr<Pathfinder::Device> device, std::shared_ptr<Pathfinder::Queue> queue);

    ~VideoPlayerFfmpeg() override;

    std::shared_ptr<AVFrame> getFrame();

    void play(const std::string &playUrl, bool forceSoftwareDecoding) override;

    void update(float dt) override;

    void render(std::shared_ptr<Pathfinder::Texture> target) override;

    void stop() override;

    void set_muted(bool muted) override;

    std::string capture_jpeg() override;

    bool start_mp4_recording() override;
    std::string stop_mp4_recording() const override;

    bool start_gif_recording() override;
    std::string stop_gif_recording() const override;

    std::shared_ptr<FfmpegDecoder> getDecoder() const;

protected:
    std::shared_ptr<FfmpegDecoder> decoder;

    std::queue<std::shared_ptr<AVFrame>> videoFrameQueue;

    SDL_AudioStream *stream{};

    std::mutex mtx;

    std::thread decodeThread;
    std::mutex decodeResMtx; // Resource mutex

    std::thread analysisThread;
    std::mutex analysisResMtx; // Resource mutex

    std::shared_ptr<AVFrame> lastFrame_;

    bool enableAudio();

    void disableAudio();

    void start_local_decoded_frame_recording();

    void stop_local_decoded_frame_recording();

    void record_local_decoded_frame(const std::shared_ptr<AVFrame> &frame);

    std::shared_ptr<Mp4Encoder> mp4Encoder_;

    std::shared_ptr<GifEncoder> gifEncoder_;

    std::ofstream localDecodedFrameTsv_;
    std::ofstream localDecodedFrameStream_;
    uint64_t localDecodedFrameIndex_ = 0;
    uint64_t localDecodedFrameOffset_ = 0;
    bool localDecodedFrameRecording_ = false;

    bool hasAudio() const;
};
