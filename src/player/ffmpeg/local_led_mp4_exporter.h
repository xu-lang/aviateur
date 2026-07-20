#pragma once

#include <string>

class LocalLedMp4Exporter {
public:
    static bool export_stream(const std::string &output_dir,
                              const std::string &stream_file_name,
                              const std::string &codec);
};
