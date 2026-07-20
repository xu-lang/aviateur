#include "video_player.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include <filesystem>
#include <future>
#include <sstream>
#include <vector>

#include "../../gui_interface.h"
#include "jpeg_encoder.h"
#include "local_led_mp4_exporter.h"

#define DEFAULT_GIF_FRAMERATE 10

VideoPlayerFfmpeg::VideoPlayerFfmpeg(std::shared_ptr<Pathfinder::Device> device,
                                     std::shared_ptr<Pathfinder::Queue> queue)
    : VideoPlayer(device, queue) {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        GuiInterface::Instance().PutLog(LogLevel::Warn, "SDL init audio failed!");
    }
}

void VideoPlayerFfmpeg::update(float dt) {
    if (should_stop_playing_) {
        return;
    }

    if (video_info_changed_) {
        yuvRenderer_->updateTextureInfo(video_width_, video_height_, video_format_);
        video_info_changed_ = false;
    }

    std::shared_ptr<AVFrame> frame = getFrame();
    if (frame && frame->linesize[0]) {
        yuvRenderer_->updateTextureData(frame);
    }
}

void VideoPlayerFfmpeg::render(std::shared_ptr<Pathfinder::Texture> target) {
    yuvRenderer_->render(target);
    GuiInterface::Instance().renderedFrameCount_.fetch_add(1, std::memory_order_relaxed);

    if (GuiInterface::Instance().local_rtp_record_raw_ &&
        GuiInterface::Instance().local_rtp_frame_index_source_ == LocalRtpFrameIndexSource::RenderedFrame) {
        record_local_decoded_frame(lastFrame_);
    }
}

std::shared_ptr<AVFrame> VideoPlayerFfmpeg::getFrame() {
    std::lock_guard lck(mtx);

    // No frame in the queue
    if (videoFrameQueue.empty()) {
        return nullptr;
    }

    // Get a frame from the queue
    std::shared_ptr<AVFrame> frame = videoFrameQueue.front();

    // Remove the frame from the queue.
    videoFrameQueue.pop();

    lastFrame_ = frame;

    return frame;
}

void VideoPlayerFfmpeg::play(const std::string &playUrl, bool forceSoftwareDecoding) {
    should_stop_playing_ = false;

    if (analysisThread.joinable()) {
        analysisThread.join();
    }

    url = playUrl;

    decoder = std::make_shared<FfmpegDecoder>();

    analysisThread = std::thread([this, forceSoftwareDecoding] {
        // Indicate we are using ffmpeg resources in a detached thread.
        analysisResMtx.lock();

        bool ok = decoder->OpenInput(url, forceSoftwareDecoding);
        if (!ok) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "Loading URL failed");
            analysisResMtx.unlock();
            return;
        }

        std::string decoder_name = decoder->hwDecoderName.has_value() ? decoder->hwDecoderName.value() : "Software";

        GuiInterface::Instance().EmitDecoderReady(decoder->GetWidth(),
                                                  decoder->GetHeight(),
                                                  decoder->GetFramerate(),
                                                  decoder_name);

        if (!isMuted && decoder->HasAudio()) {
            enableAudio();
        }

        if (decoder->HasVideo()) {
            update_video_info(decoder->GetWidth(), decoder->GetHeight(), decoder->GetVideoFrameFormat());
        }

        if (GuiInterface::Instance().local_rtp_record_raw_ &&
            GuiInterface::Instance().local_rtp_frame_index_source_ != LocalRtpFrameIndexSource::RtpFrame) {
            start_local_decoded_frame_recording();
        }

        // Bitrate callback.
        decoder->bitrateUpdateCallback = [](uint64_t bitrate) { GuiInterface::Instance().EmitBitrateUpdate(bitrate); };

        decodeThread = std::thread([this] {
            decodeResMtx.lock();

            while (!should_stop_playing_) {
                try {
                    // Getting frame.
                    auto frame = decoder->GetNextFrame();
                    if (!frame) {
                        continue;
                    }

                    // Push frame to the buffer queue.
                    std::lock_guard lck(mtx);
                    if (videoFrameQueue.size() > 10) {
                        videoFrameQueue.pop();
                    }
                    videoFrameQueue.push(frame);
                }
                // Decoder error. But continue.
                catch (const SendPacketException &e) {
                    GuiInterface::Instance().PutLog(LogLevel::Error, e.what());
                    GuiInterface::Instance().ShowTip(FTR("invalid input data"));
                }
                // Read frame error, mostly due to a lost signal. But continue.
                catch (const ReadFrameException &e) {
                    GuiInterface::Instance().PutLog(LogLevel::Error, e.what());
                    GuiInterface::Instance().ShowTip(FTR("signal lost"));
                }
                // Break on other unknown errors.
                catch (const std::exception &e) {
                    GuiInterface::Instance().PutLog(LogLevel::Error, e.what());
                    break;
                }
            }

            decodeResMtx.unlock();
        });

        // Start decode thread.
        decodeThread.detach();

        // We are done with ffmpeg resources.
        analysisResMtx.unlock();
    });

    // Start analysis thread.
    analysisThread.detach();
}

void VideoPlayerFfmpeg::stop() {
    should_stop_playing_ = true;
    stop_local_decoded_frame_recording();

    if (decoder && decoder->pFormatCtx) {
        decoder->pFormatCtx->interrupt_callback.callback = [](void *) { return 1; };
    }

    // The thread will be unjoinable after calling detach().
    // if (analysisThread.joinable()) {
    //     analysisThread.join();
    // }
    // if (decodeThread.joinable()) {
    //     decodeThread.join();
    // }

    // Wait until the detached threads finish.
    {
        std::lock_guard lck1(analysisResMtx);
        std::lock_guard lck2(decodeResMtx);
    }

    {
        std::lock_guard lck(mtx);
        videoFrameQueue = std::queue<std::shared_ptr<AVFrame>>();
    }

    // Do this before closing input.
    disableAudio();

    if (decoder) {
        decoder->CloseInput();
        decoder.reset();
    }
}

void VideoPlayerFfmpeg::start_local_decoded_frame_recording() {
    stop_local_decoded_frame_recording();

    const auto output_dir = std::filesystem::current_path().string() + "/";
    const auto stream_file_name = "frames.yuv";
    localDecodedFrameTsv_.open(output_dir + "frames.tsv", std::ios::out | std::ios::trunc);
    localDecodedFrameStream_.open(output_dir + stream_file_name, std::ios::binary | std::ios::trunc);
    localDecodedFrameOffset_ = 0;
    localDecodedFrameIndex_ = 0;
    localDecodedFrameTsv_ << "frame_index\treceived_ms\trtp_timestamp\tlast_seq\tpacket_count\tled_on\toffset\tbytes\twidth\theight\tpix_fmt\n";
    localDecodedFrameRecording_ = localDecodedFrameTsv_.is_open() && localDecodedFrameStream_.is_open();

    if (decoder && GuiInterface::Instance().local_rtp_frame_index_source_ == LocalRtpFrameIndexSource::DecodedFrame) {
        decoder->decodedFrameRecordCallback = [this](const std::shared_ptr<AVFrame> &frame) {
            record_local_decoded_frame(frame);
        };
    }
}

void VideoPlayerFfmpeg::stop_local_decoded_frame_recording() {
    const bool should_export = localDecodedFrameRecording_ && localDecodedFrameIndex_ > 0;
    if (decoder) {
        decoder->decodedFrameRecordCallback = nullptr;
    }
    localDecodedFrameRecording_ = false;
    if (localDecodedFrameTsv_.is_open()) {
        localDecodedFrameTsv_.close();
    }
    if (localDecodedFrameStream_.is_open()) {
        localDecodedFrameStream_.close();
    }
    if (should_export) {
        LocalLedMp4Exporter::export_stream(std::filesystem::current_path().string() + "/", "frames.yuv", "YUV");
    }
}

void VideoPlayerFfmpeg::record_local_decoded_frame(const std::shared_ptr<AVFrame> &frame) {
    if (!localDecodedFrameRecording_ || !frame || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    const auto pix_fmt = static_cast<AVPixelFormat>(frame->format);
    if (pix_fmt != AV_PIX_FMT_YUV420P && pix_fmt != AV_PIX_FMT_YUVJ420P && pix_fmt != AV_PIX_FMT_NV12) {
        return;
    }

    const uint64_t received_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    const auto frame_offset = localDecodedFrameOffset_;

    const int frame_size = av_image_get_buffer_size(pix_fmt, frame->width, frame->height, 1);
    if (frame_size <= 0) {
        return;
    }
    std::vector<uint8_t> frame_data(frame_size);
    const int copied = av_image_copy_to_buffer(frame_data.data(),
                                               static_cast<int>(frame_data.size()),
                                               frame->data,
                                               frame->linesize,
                                               pix_fmt,
                                               frame->width,
                                               frame->height,
                                               1);
    if (copied <= 0) {
        return;
    }
    frame_data.resize(copied);

    localDecodedFrameStream_.write(reinterpret_cast<const char *>(frame_data.data()), frame_data.size());
    localDecodedFrameStream_.flush();
    localDecodedFrameOffset_ += frame_data.size();

    const int led_on = GuiInterface::Instance().led_on_.load(std::memory_order_relaxed) ? 1 : 0;
    uint64_t frame_index = localDecodedFrameIndex_;
    if (GuiInterface::Instance().local_rtp_frame_index_source_ == LocalRtpFrameIndexSource::RenderedFrame) {
        frame_index = GuiInterface::Instance().renderedFrameCount_.load(std::memory_order_relaxed);
    }

    localDecodedFrameTsv_ << frame_index << '\t' << received_ms << '\t' << 0 << '\t' << 0 << '\t' << 1
                           << '\t' << led_on << '\t' << frame_offset << '\t' << frame_data.size() << '\t'
                           << frame->width << '\t' << frame->height << '\t' << av_get_pix_fmt_name(pix_fmt) << '\n';
    localDecodedFrameTsv_.flush();
    localDecodedFrameIndex_++;
}

void VideoPlayerFfmpeg::set_muted(bool muted) {
    if (!decoder->HasAudio()) {
        return;
    }

    if (!muted && decoder) {
        decoder->ClearAudioBuff();

        if (!enableAudio()) {
            return;
        }
    } else {
        disableAudio();
    }

    isMuted = muted;
    // emit onMutedChanged(muted);
}

VideoPlayerFfmpeg::~VideoPlayerFfmpeg() {
    stop();

    SDL_Quit();
}

std::string VideoPlayerFfmpeg::capture_jpeg() {
    if (!lastFrame_) {
        return "";
    }

    auto dir = GuiInterface::GetCaptureDir();

    try {
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
    }

    std::stringstream filePath;
    filePath << dir;
    filePath << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << ".jpg";

    std::ofstream outfile(filePath.str());
    outfile.close();

    auto ok = JpegEncoder::encodeJpeg(filePath.str(), lastFrame_);

    return ok ? std::string(filePath.str()) : "";
}

bool VideoPlayerFfmpeg::start_mp4_recording() {
    if (should_stop_playing_ && !lastFrame_) {
        return false;
    }

    auto dir = GuiInterface::GetCaptureDir();

    try {
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
    }

    std::stringstream filePath;
    filePath << dir;
    filePath << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << ".mp4";

    std::ofstream outfile(filePath.str());
    outfile.close();

    mp4Encoder_ = std::make_shared<Mp4Encoder>(filePath.str());

    // Audio track not handled for now.
    if (decoder->HasAudio()) {
        mp4Encoder_->addTrack(decoder->pFormatCtx->streams[decoder->audioStreamIndex]);
    }

    // Add video track.
    if (decoder->HasVideo()) {
        mp4Encoder_->addTrack(decoder->pFormatCtx->streams[decoder->videoStreamIndex]);
    }

    if (!mp4Encoder_->start()) {
        return false;
    }

    // 设置获得NALU回调
    decoder->gotPktCallback = [this](const std::shared_ptr<AVPacket> &packet) {
        // 输入编码器
        mp4Encoder_->writePacket(packet, packet->stream_index == decoder->videoStreamIndex);
    };

    return true;
}

std::string VideoPlayerFfmpeg::stop_mp4_recording() const {
    if (!mp4Encoder_) {
        return {};
    }
    mp4Encoder_->stop();
    decoder->gotPktCallback = nullptr;

    return mp4Encoder_->saveFilePath_;
}

bool VideoPlayerFfmpeg::start_gif_recording() {
    if (should_stop_playing_) {
        return false;
    }

    if (!(decoder && decoder->HasVideo())) {
        return false;
    }

    std::stringstream gif_file_path;
    gif_file_path << "recording/";
    gif_file_path << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()
                  << ".gif";

    gifEncoder_ = std::make_shared<GifEncoder>();

    if (!gifEncoder_->open(decoder->width,
                           decoder->height,
                           decoder->GetVideoFrameFormat(),
                           DEFAULT_GIF_FRAMERATE,
                           gif_file_path.str())) {
        return false;
    }

    // 设置获得解码帧回调
    decoder->gotVideoFrameCallback = [this](const std::shared_ptr<AVFrame> &frame) {
        if (!gifEncoder_) {
            return;
        }
        if (!gifEncoder_->isOpened()) {
            return;
        }
        // 根据GIF帧率跳帧
        uint64_t now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (gifEncoder_->getLastEncodeTime() + 1000 / gifEncoder_->getFrameRate() > now) {
            return;
        }

        gifEncoder_->encodeFrame(frame);
    };

    return true;
}

std::string VideoPlayerFfmpeg::stop_gif_recording() const {
    decoder->gotVideoFrameCallback = nullptr;
    if (!gifEncoder_) {
        return "";
    }
    gifEncoder_->close();
    return gifEncoder_->_saveFilePath;
}

std::shared_ptr<FfmpegDecoder> VideoPlayerFfmpeg::getDecoder() const {
    return decoder;
}

void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    if (additional_amount > 0) {
        Uint8 *data = SDL_stack_alloc(Uint8, additional_amount);
        if (data) {
            auto *player = static_cast<VideoPlayerFfmpeg *>(userdata);

            int ret = player->getDecoder()->ReadAudioBuff(data, additional_amount);

            if (ret) {
                SDL_PutAudioStreamData(stream, data, additional_amount);
                SDL_stack_free(data);
            }
        }
    }
}

bool VideoPlayerFfmpeg::enableAudio() {
    if (!decoder) {
        return false;
    }
    if (!decoder->HasAudio()) {
        return false;
    }
    if (stream) {
        GuiInterface::Instance().PutLog(LogLevel::Warn, "Audio stream already exists!");
        return false;
    }

    const SDL_AudioSpec spec = {SDL_AUDIO_S16, decoder->GetAudioChannelCount(), decoder->GetAudioSampleRate()};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, this);
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));

    return true;
}

void VideoPlayerFfmpeg::disableAudio() {
    if (stream) {
        SDL_CloseAudioDevice(SDL_GetAudioStreamDevice(stream));
        stream = nullptr;
    }
}

bool VideoPlayerFfmpeg::hasAudio() const {
    if (!decoder) {
        return false;
    }

    return decoder->HasAudio();
}
