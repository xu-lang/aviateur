#include "local_led_mp4_exporter.h"

#include "ffmpeg_include.h"
#include "../../gui_interface.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#endif

extern "C" {
#include <libavutil/opt.h>
}

namespace {

struct FrameMeta {
    uint64_t received_ms = 0;
    bool led_on = false;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    int width = 0;
    int height = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
};

std::vector<std::string> SplitTabs(const std::string &line) {
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    return parts;
}

std::vector<FrameMeta> ReadFrameMeta(const std::string &path) {
    std::vector<FrameMeta> frames;
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line)) {
        return frames;
    }

    while (std::getline(file, line)) {
        const auto parts = SplitTabs(line);
        if (parts.size() < 8) {
            continue;
        }

        FrameMeta meta;
        meta.received_ms = std::stoull(parts[1]);
        meta.led_on = std::stoi(parts[5]) != 0;
        meta.offset = std::stoull(parts[6]);
        meta.bytes = std::stoull(parts[7]);
        if (parts.size() >= 11) {
            meta.width = std::stoi(parts[8]);
            meta.height = std::stoi(parts[9]);
            meta.pix_fmt = av_get_pix_fmt(parts[10].c_str());
        }
        frames.push_back(meta);
    }
    return frames;
}

std::string HackPositionName() {
    switch (GuiInterface::Instance().local_rtp_frame_index_source_) {
        case LocalRtpFrameIndexSource::RtpFrame:
            return "rtp_received";
        case LocalRtpFrameIndexSource::DecodedFrame:
            return "frame_decoded";
        case LocalRtpFrameIndexSource::RenderedFrame:
            return "frame_rendered";
    }
    return "unknown";
}

int EstimateFrameRate(const std::vector<FrameMeta> &frames) {
    if (frames.size() < 2) {
        return 30;
    }

    uint64_t duration_ms = 0;
    size_t count = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].received_ms > frames[i - 1].received_ms) {
            duration_ms += frames[i].received_ms - frames[i - 1].received_ms;
            count++;
        }
    }
    if (count == 0 || duration_ms == 0) {
        return 30;
    }

    const double avg_ms = static_cast<double>(duration_ms) / static_cast<double>(count);
    return std::clamp(static_cast<int>(std::lround(1000.0 / avg_ms)), 1, 120);
}

void DrawLedBlock(AVFrame *frame) {
    if (!frame || frame->format != AV_PIX_FMT_YUV420P) {
        return;
    }

    const int block = (std::max)(16, (std::min)(frame->width, frame->height) / 12);
    const int width = (std::min)(block, frame->width);
    const int height = (std::min)(block, frame->height);

    for (int y = 0; y < height; ++y) {
        std::fill_n(frame->data[0] + y * frame->linesize[0], width, 149);
    }

    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    for (int y = 0; y < chroma_height; ++y) {
        std::fill_n(frame->data[1] + y * frame->linesize[1], chroma_width, 43);
        std::fill_n(frame->data[2] + y * frame->linesize[2], chroma_width, 21);
    }
}

class Mp4Writer {
public:
    ~Mp4Writer() {
        close();
    }

    bool open(const std::string &path, int width, int height, int fps) {
        fps_ = fps;
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "H.264 encoder not found for LED MP4 export");
            return false;
        }

        if (avformat_alloc_output_context2(&format_ctx_, nullptr, "mp4", path.c_str()) < 0 || !format_ctx_) {
            return false;
        }

        stream_ = avformat_new_stream(format_ctx_, nullptr);
        if (!stream_) {
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            return false;
        }

        codec_ctx_->codec_id = AV_CODEC_ID_H264;
        codec_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx_->width = width;
        codec_ctx_->height = height;
        codec_ctx_->time_base = AVRational{1, fps_};
        codec_ctx_->framerate = AVRational{fps_, 1};
        codec_ctx_->gop_size = fps_;
        codec_ctx_->max_b_frames = 0;
        codec_ctx_->bit_rate = static_cast<int64_t>(width) * height * 3;
        if ((format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        av_opt_set(codec_ctx_->priv_data, "preset", "veryfast", 0);

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            return false;
        }
        if (avcodec_parameters_from_context(stream_->codecpar, codec_ctx_) < 0) {
            return false;
        }
        stream_->time_base = codec_ctx_->time_base;

        if (avio_open(&format_ctx_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            return false;
        }
        if (avformat_write_header(format_ctx_, nullptr) < 0) {
            return false;
        }

        path_ = path;
        opened_ = true;
        return true;
    }

    bool write(AVFrame *frame) {
        frame->pts = next_pts_++;
        if (avcodec_send_frame(codec_ctx_, frame) < 0) {
            return false;
        }
        return drain(false);
    }

    void close() {
        if (!opened_) {
            cleanup();
            return;
        }
        drain(true);
        av_write_trailer(format_ctx_);
        GuiInterface::Instance().PutLog(LogLevel::Info, "Exported LED MP4: {}", path_);
        opened_ = false;
        cleanup();
    }

private:
    bool drain(bool flush) {
        if (flush) {
            avcodec_send_frame(codec_ctx_, nullptr);
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            return false;
        }

        while (true) {
            const int ret = avcodec_receive_packet(codec_ctx_, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                av_packet_free(&packet);
                return false;
            }
            av_packet_rescale_ts(packet, codec_ctx_->time_base, stream_->time_base);
            packet->stream_index = stream_->index;
            av_interleaved_write_frame(format_ctx_, packet);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        return true;
    }

    void cleanup() {
        if (format_ctx_ && format_ctx_->pb) {
            avio_closep(&format_ctx_->pb);
        }
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
        }
        if (format_ctx_) {
            avformat_free_context(format_ctx_);
            format_ctx_ = nullptr;
        }
    }

    AVFormatContext *format_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVStream *stream_ = nullptr;
    int fps_ = 30;
    int64_t next_pts_ = 0;
    bool opened_ = false;
    std::string path_;
};

#ifdef _WIN32
std::string QuoteCommandArg(const std::string &value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"') {
            quoted += "\\\"";
        } else {
            quoted += c;
        }
    }
    quoted += "\"";
    return quoted;
}

std::string FindSystemFfmpeg() {
    const std::vector<std::string> candidates = {
        "D:/green-sw/ffmpeg/bin/ffmpeg.exe",
        "D:/green-sw/ffmpeg/bin/ffmpeg",
        "ffmpeg.exe",
        "ffmpeg",
    };
    for (const auto &candidate : candidates) {
        if (candidate.find('/') == std::string::npos || std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool WriteYuv420Frame(std::ofstream &output, AVFrame *frame) {
    if (!frame || frame->format != AV_PIX_FMT_YUV420P) {
        return false;
    }

    const int chroma_width = (frame->width + 1) / 2;
    const int chroma_height = (frame->height + 1) / 2;
    for (int y = 0; y < frame->height; ++y) {
        output.write(reinterpret_cast<const char *>(frame->data[0] + y * frame->linesize[0]), frame->width);
    }
    for (int y = 0; y < chroma_height; ++y) {
        output.write(reinterpret_cast<const char *>(frame->data[1] + y * frame->linesize[1]), chroma_width);
    }
    for (int y = 0; y < chroma_height; ++y) {
        output.write(reinterpret_cast<const char *>(frame->data[2] + y * frame->linesize[2]), chroma_width);
    }
    return output.good();
}

int RunProcess(const std::string &command) {
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    std::vector<char> command_line(command.begin(), command.end());
    command_line.push_back('\0');

    if (!CreateProcessA(nullptr,
                        command_line.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        nullptr,
                        &startup_info,
                        &process_info)) {
        return static_cast<int>(GetLastError());
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

class RawYuvMp4Writer {
public:
    ~RawYuvMp4Writer() {
        close();
    }

    bool open(const std::string &path, int width, int height, int fps) {
        path_ = path;
        width_ = width;
        height_ = height;
        fps_ = fps;
        auto temp_path = std::filesystem::path(path);
        temp_path.replace_extension(".yuv");
        temp_path_ = temp_path.string();
        output_.open(temp_path_, std::ios::binary | std::ios::trunc);
        opened_ = output_.is_open();
        if (!opened_) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "Failed to open temporary YUV file: {}", temp_path_);
        }
        return opened_;
    }

    bool write(AVFrame *frame) {
        return opened_ && WriteYuv420Frame(output_, frame);
    }

    void close() {
        if (!opened_) {
            return;
        }
        output_.close();
        opened_ = false;

        const auto ffmpeg = FindSystemFfmpeg();
        if (ffmpeg.empty()) {
            GuiInterface::Instance().PutLog(LogLevel::Error, "ffmpeg.exe not found for LED MP4 export");
            return;
        }

        const std::string size = std::to_string(width_) + "x" + std::to_string(height_);
        const std::string command = fmt::format(
            "{} -y -f rawvideo -pix_fmt yuv420p -s {} -r {} -i {} -c:v libx264 -pix_fmt yuv420p -movflags +faststart {}",
            QuoteCommandArg(ffmpeg),
            size,
            fps_,
            QuoteCommandArg(temp_path_),
            QuoteCommandArg(path_));

        const int result = RunProcess(command);
        if (result == 0) {
            encoded_ = true;
            GuiInterface::Instance().PutLog(LogLevel::Info, "Exported LED MP4: {}", path_);
            std::error_code ec;
            std::filesystem::remove(temp_path_, ec);
        } else {
            GuiInterface::Instance().PutLog(LogLevel::Error, "ffmpeg LED MP4 export failed with exit code {}", result);
        }
    }

    bool encoded() const {
        return encoded_;
    }

private:
    std::ofstream output_;
    std::string path_;
    std::string temp_path_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    bool opened_ = false;
    bool encoded_ = false;
};
#endif

std::shared_ptr<AVFrame> ToYuv420Frame(AVFrame *src,
                                       SwsContext *&sws_ctx,
                                       std::vector<uint8_t> &buffer) {
    auto frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *f) { av_frame_free(&f); });
    if (!frame) {
        return nullptr;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = src->width;
    frame->height = src->height;
    const int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
    if (size <= 0) {
        return nullptr;
    }
    buffer.resize(size);
    av_image_fill_arrays(frame->data, frame->linesize, buffer.data(), AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);

    if (static_cast<AVPixelFormat>(src->format) == AV_PIX_FMT_YUV420P) {
        av_image_copy(frame->data,
                      frame->linesize,
                      const_cast<const uint8_t **>(src->data),
                      src->linesize,
                      AV_PIX_FMT_YUV420P,
                      frame->width,
                      frame->height);
        return frame;
    }

    sws_ctx = sws_getCachedContext(sws_ctx,
                                   src->width,
                                   src->height,
                                   static_cast<AVPixelFormat>(src->format),
                                   frame->width,
                                   frame->height,
                                   AV_PIX_FMT_YUV420P,
                                   SWS_BICUBIC,
                                   nullptr,
                                   nullptr,
                                   nullptr);
    if (!sws_ctx) {
        return nullptr;
    }
    sws_scale(sws_ctx, src->data, src->linesize, 0, src->height, frame->data, frame->linesize);
    return frame;
}

bool ExportRawVideo(const std::string &input_path,
                    const std::string &output_path,
                    const std::string &codec_name,
                    const std::vector<FrameMeta> &meta,
                    int fps) {
    const AVInputFormat *input_format = av_find_input_format(codec_name == "H265" ? "hevc" : "h264");
    AVFormatContext *format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, input_path.c_str(), input_format, nullptr) < 0) {
        return false;
    }

    AVCodecParameters *codecpar = format_ctx->streams[0]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext *decoder_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(decoder_ctx, codecpar);
    if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&format_ctx);
        return false;
    }

#ifdef _WIN32
    RawYuvMp4Writer writer;
#else
    Mp4Writer writer;
#endif
    bool writer_opened = false;
    size_t frame_index = 0;
    SwsContext *sws_ctx = nullptr;
    std::vector<uint8_t> yuv_buffer;
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();

    auto handle_decoded = [&] {
        if (!writer_opened) {
            writer_opened = writer.open(output_path, decoded->width, decoded->height, fps);
            if (!writer_opened) {
                return false;
            }
        }
        auto yuv = ToYuv420Frame(decoded, sws_ctx, yuv_buffer);
        if (!yuv) {
            return false;
        }
        if (frame_index < meta.size() && meta[frame_index].led_on) {
            DrawLedBlock(yuv.get());
        }
        writer.write(yuv.get());
        frame_index++;
        return true;
    };

    while (av_read_frame(format_ctx, packet) >= 0) {
        if (avcodec_send_packet(decoder_ctx, packet) == 0) {
            while (avcodec_receive_frame(decoder_ctx, decoded) == 0) {
                if (!handle_decoded()) {
                    break;
                }
                av_frame_unref(decoded);
            }
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder_ctx, nullptr);
    while (avcodec_receive_frame(decoder_ctx, decoded) == 0) {
        handle_decoded();
        av_frame_unref(decoded);
    }

    writer.close();
    sws_freeContext(sws_ctx);
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&format_ctx);
#ifdef _WIN32
    return writer_opened && writer.encoded();
#else
    return writer_opened;
#endif
}

bool ExportYuv(const std::string &input_path,
               const std::string &output_path,
               const std::vector<FrameMeta> &meta,
               int fps) {
    if (meta.empty() || meta[0].width <= 0 || meta[0].height <= 0 || meta[0].pix_fmt == AV_PIX_FMT_NONE) {
        return false;
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

#ifdef _WIN32
    RawYuvMp4Writer writer;
#else
    Mp4Writer writer;
#endif
    if (!writer.open(output_path, meta[0].width, meta[0].height, fps)) {
        return false;
    }

    SwsContext *sws_ctx = nullptr;
    std::vector<uint8_t> yuv420_buffer;
    for (size_t i = 0; i < meta.size(); ++i) {
        const auto &m = meta[i];
        if (m.bytes == 0 || m.width != meta[0].width || m.height != meta[0].height || m.pix_fmt == AV_PIX_FMT_NONE) {
            continue;
        }

        std::vector<uint8_t> input_buffer(m.bytes);
        input.seekg(static_cast<std::streamoff>(m.offset), std::ios::beg);
        input.read(reinterpret_cast<char *>(input_buffer.data()), static_cast<std::streamsize>(input_buffer.size()));
        if (input.gcount() != static_cast<std::streamsize>(input_buffer.size())) {
            break;
        }

        AVFrame src{};
        src.format = m.pix_fmt;
        src.width = m.width;
        src.height = m.height;
        if (av_image_fill_arrays(src.data, src.linesize, input_buffer.data(), m.pix_fmt, m.width, m.height, 1) < 0) {
            continue;
        }

        auto yuv = ToYuv420Frame(&src, sws_ctx, yuv420_buffer);
        if (!yuv) {
            continue;
        }
        if (m.led_on) {
            DrawLedBlock(yuv.get());
        }
        writer.write(yuv.get());
    }

    writer.close();
    sws_freeContext(sws_ctx);
#ifdef _WIN32
    return writer.encoded();
#else
    return true;
#endif
}

} // namespace

bool LocalLedMp4Exporter::export_stream(const std::string &output_dir,
                                        const std::string &stream_file_name,
                                        const std::string &codec) {
    const auto input_path = output_dir + stream_file_name;
    const auto tsv_path = output_dir + "frames.tsv";
    const auto output_path = output_dir + "frames_led_" + HackPositionName() + ".mp4";
    if (!std::filesystem::exists(input_path) || !std::filesystem::exists(tsv_path)) {
        return false;
    }

    const auto meta = ReadFrameMeta(tsv_path);
    if (meta.empty()) {
        GuiInterface::Instance().PutLog(LogLevel::Warn, "Skip LED MP4 export: no frame metadata");
        return false;
    }

    GuiInterface::Instance().PutLog(LogLevel::Info, "Exporting LED MP4: {}", output_path);
    const int fps = EstimateFrameRate(meta);
    if (codec == "YUV") {
        return ExportYuv(input_path, output_path, meta, fps);
    }
    return ExportRawVideo(input_path, output_path, codec, meta, fps);
}
