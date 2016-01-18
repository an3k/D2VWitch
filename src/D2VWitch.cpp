/*

Copyright (c) 2016, John Smith

Permission to use, copy, modify, and/or distribute this software for
any purpose with or without fee is hereby granted, provided that the
above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

*/


#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}


#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif


#include "Bullshit.h"
#include "D2V.h"
#include "FakeFile.h"
#include "FFMPEG.h"


void printProgress(int64_t current_position, int64_t total_size) {
    fprintf(stderr, "%3d%%\r", (int)(current_position * 100 / total_size));
}


void printWarnings(const std::string &message) {
    fprintf(stderr, "%s\n", message.c_str());
}


AVStream *selectVideoStreamById(AVFormatContext *fctx, int id) {
    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (fctx->streams[i]->id == id) {
                fctx->streams[i]->discard = AVDISCARD_DEFAULT;
                return fctx->streams[i];
            }
        }
    }

    return nullptr;
}


AVStream *selectFirstVideoStream(AVFormatContext *fctx) {
    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            fctx->streams[i]->discard = AVDISCARD_DEFAULT;
            return fctx->streams[i];
        }
    }

    return nullptr;
}


bool selectAudioStreamsById(AVFormatContext *fctx, std::vector<int> &audio_ids) {
    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            for (size_t j = 0; j < audio_ids.size(); j++) {
                if (fctx->streams[i]->id == audio_ids[j]) {
                    fctx->streams[i]->discard = AVDISCARD_DEFAULT;
                    audio_ids.erase(audio_ids.begin() + j);
                    break;
                }
            }
        }
    }

    return !audio_ids.size();
}


bool selectAllAudioStreams(AVFormatContext *fctx) {
    bool okay = false;

    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            fctx->streams[i]->discard = AVDISCARD_DEFAULT;

            okay = true;
        }
    }

    return okay;
}


void deselectAllStreams(AVFormatContext *fctx) {
    for (unsigned i = 0; i < fctx->nb_streams; i++)
        fctx->streams[i]->discard = AVDISCARD_ALL;
}


void printHelp() {
    const char usage[] = R"usage(
D2V Witch indexes MPEG (1, 2) streams and writes D2V files. These can
be used with the VapourSynth plugin d2vsource.

Usage: D2VWitch [options] input_file1 input_file2 ...

Options:
    --help
        Display this message.

    --version
        Display version numbers and ffmpeg configuration.

    --info
        Display relevant information about the input file(s), such as a
        list of all the video and audio tracks and their ids.

    --quiet
        Do not print progress information or warnings. Fatal errors are
        always printed.

    --output <d2v name>
        Specify the name of the D2V file. The special name "-" means
        standard output. If not specified, the name of the D2V file is
        deduced from the name of the first input file.

    --audio-ids <id1,id2,...>
        Demux the audio tracks with the specified ids. The special
        value "all" means that all audio tracks should be demuxed. By
        default, no audio tracks are demuxed. The names of the audio
        files are deduced from the name of the D2V file.

    --video-id <id>
        Process the video track with this id. By default, the first
        video track found will be processed.

)usage";

    fprintf(stderr, "%s", usage);
}


void printVersions() {
    unsigned lavf = avformat_version();
    unsigned lavc = avcodec_version();
    unsigned lavu = avutil_version();

    fprintf(stderr,
            "D2V Witch version: %s\n"
            "libavformat version: %u.%u.%u\n"
            "libavcodec version: %u.%u.%u\n"
            "libavutil version: %u.%u.%u\n"
            "\n"
            "libavformat configuration:\n%s\n\n"
            "libavcodec configuration:\n%s\n\n"
            "libavutil configuration:\n%s\n\n",
            PACKAGE_VERSION,
            (lavf >> 16) & 0xff, (lavf >> 8) & 0xff, lavf & 0xff,
            (lavc >> 16) & 0xff, (lavc >> 8) & 0xff, lavc & 0xff,
            (lavu >> 16) & 0xff, (lavu >> 8) & 0xff, lavu & 0xff,
            avformat_configuration(),
            avcodec_configuration(),
            avutil_configuration());
}


void printInfo(const AVFormatContext *fctx, const FakeFile &fake_file) {
    fprintf(stderr, "Input file(s):\n");
    for (size_t i = 0; i < fake_file.size(); i++)
        fprintf(stderr, "    %s\n", fake_file[i].name.c_str());

    fprintf(stderr, "\n    Type: %s\n", fctx->iformat->long_name ? fctx->iformat->long_name : fctx->iformat->name);

    fprintf(stderr, "\nVideo tracks:\n");

    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            const char *type = "unknown";
            const AVCodecDescriptor *desc = av_codec_get_codec_descriptor(fctx->streams[i]->codec);
            if (desc)
                type = desc->long_name ? desc->long_name : desc->name;

            int width, height;
            if (av_opt_get_image_size(fctx->streams[i]->codec, "video_size", 0, &width, &height) < 0)
                width = height = -1;

            fprintf(stderr, "    Id: %x, type: %s, %dx%d\n",
                    fctx->streams[i]->id,
                    type,
                    width,
                    height);
        }
    }

    fprintf(stderr, "\nAudio tracks:\n");

    for (unsigned i = 0; i < fctx->nb_streams; i++) {
        if (fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            const char *type = "unknown";
            const AVCodecDescriptor *desc = av_codec_get_codec_descriptor(fctx->streams[i]->codec);
            if (desc)
                type = desc->long_name ? desc->long_name : desc->name;

            int64_t bit_rate, channel_layout, sample_rate;

            if (av_opt_get_int(fctx->streams[i]->codec, "ab", 0, &bit_rate) < 0)
                bit_rate = -1;

            if (av_opt_get_int(fctx->streams[i]->codec, "channel_layout", 0, &channel_layout) < 0)
                channel_layout = 0;

            if (av_opt_get_int(fctx->streams[i]->codec, "ar", 0, &sample_rate) < 0)
                sample_rate = -1;

            char channels[512] = { 0 };
            av_get_channel_layout_string(channels, 512, 0, channel_layout);

            fprintf(stderr, "    Id: %x, type: %s, %d kbps, %s, %d Hz\n",
                    fctx->streams[i]->id,
                    type,
                    (int)(bit_rate / 1000),
                    channels,
                    (int)sample_rate);
        }
    }
}


struct CommandLine {
    bool help_wanted;

    bool version_wanted;

    bool info_wanted;

    bool stay_quiet;

    std::string d2v_path;

    std::vector<int> audio_ids;
    bool audio_ids_all;

    int video_id;
    bool have_video_id;

    std::string error;

    CommandLine()
        : help_wanted(false)
        , version_wanted(false)
        , info_wanted(false)
        , stay_quiet(false)
        , d2v_path{ }
        , audio_ids{ }
        , audio_ids_all(false)
        , video_id(0)
        , have_video_id(false)
        , error{ }
    { }

    const std::string &getError() const {
        return error;
    }

    // char** or std::vector<std::string>
    template<typename Args>
    bool parse(int argc, Args argv, FakeFile &fake_file) {
        const char *opt_help = "--help";
        const char *opt_version = "--version";
        const char *opt_info = "--info";
        const char *opt_quiet = "--quiet";
        const char *opt_output = "--output";
        const char *opt_audio_ids = "--audio-ids";
        const char *opt_video_id = "--video-id";

        std::unordered_set<std::string> valid_options = {
            opt_help,
            opt_version,
            opt_info,
            opt_quiet,
            opt_output,
            opt_audio_ids,
            opt_video_id
        };

        for (int i = 1; i < argc; i++) {
            std::string arg(argv[i]);
            if (arg.size() > 1 && arg[0] == '-' && !valid_options.count(arg)) {
                error = "Unknown option '" + arg + "'. If this is the name of a file, please pass it as './" + arg + "'.";
                return false;
            }
        }

        for (int i = 1; i < argc; i++) {
            std::string arg(argv[i]);

            if (arg == opt_help) {
                help_wanted = true;
                return true;
            } else if (arg == opt_version) {
                version_wanted = true;
                return true;
            } else if (arg == opt_info) {
                info_wanted = true;
            } else if (arg == opt_quiet) {
                stay_quiet = true;
            } else if (arg == opt_output) {
                if (i == argc - 1 || valid_options.count(argv[i + 1])) {
                    error = opt_output;
                    error += " requires a file name.";
                    return false;
                }

                d2v_path = argv[i + 1];
                i++;
            } else if (arg == opt_audio_ids) {
                if (i == argc - 1 || valid_options.count(argv[i + 1])) {
                    error = opt_audio_ids;
                    error += " requires a list of audio track ids, or the special value 'all'.";
                    return false;
                }

                std::string ids(argv[i + 1]);
                i++;

                if (ids == "all") {
                    audio_ids_all = true;
                } else {
                    size_t id_start = 0, id_end;

                    do {
                        id_end = ids.find(',', id_start);
                        std::string id;
                        if (id_end == std::string::npos)
                            id = ids.substr(id_start);
                        else
                            id = ids.substr(id_start, id_end - id_start);

                        size_t converted_chars;
                        try {
                            audio_ids.push_back(std::stoi(id, &converted_chars, 16));
                        } catch (...) {
                            error = "Invalid audio id '" + id + "'.";
                            return false;
                        }

                        if (id.size() != converted_chars) {
                            error = "Audio id '" + id + "' is not a valid hexadecimal number.";
                            return false;
                        }

                        id_start = id_end + 1;
                    } while (id_end != std::string::npos);
                }
            } else if (arg == opt_video_id) {
                if (i == argc - 1 || valid_options.count(argv[i + 1])) {
                    error = opt_video_id;
                    error += " requires a video id.";
                    return false;
                }

                std::string id(argv[i + 1]);
                i++;

                size_t converted_chars;
                try {
                    video_id = std::stoi(id, &converted_chars, 16);
                } catch (...) {
                    error = "Invalid video id '" + id + "'.";
                    return false;
                }

                if (id.size() != converted_chars) {
                    error = "Video id '" + id + "' is not a valid hexadecimal number.";
                    return false;
                }
            } else { // Input files.
                std::string err;
                makeAbsolute(arg, err);
                if (err.size()) {
                    error = "Failed to turn '" + arg + "' into an absolute path: " + err;
                    return false;
                }

                fake_file.push_back(arg);
            }
        }

        if (!fake_file.size()) {
            error = "No files given. Try '--help'.";
            return false;
        }

        return true;
    }
};


#ifdef _WIN32
BOOL WINAPI HandlerRoutine(DWORD dwCtrlType) {
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        _exit(1);
    default:
        return FALSE;
    }
}


int wmain(int argc, wchar_t **argvw) {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        fprintf(stderr, "Failed to set stdout to binary mode.\n");

    SetConsoleCtrlHandler(HandlerRoutine, TRUE);

    UTF16 utf16;

    std::vector<std::string> argv;

    for (int i = 0; i < argc; i++)
        argv.push_back(utf16.to_bytes(argvw[i]));
#else
int main(int argc, char **argv) {
#endif

    // ffmpeg init part 0
    av_log_set_level(AV_LOG_PANIC);
    av_register_all();
    avcodec_register_all();


    // command line parsing
    FakeFile fake_file;

    CommandLine cmd;
    if (!cmd.parse(argc, argv, fake_file)) {
        fprintf(stderr, "%s\n", cmd.getError().c_str());
        return 1;
    }

    if (cmd.help_wanted) {
        printHelp();
        return 0;
    }

    if (cmd.version_wanted) {
        printVersions();
        return 0;
    }


    // input opening
    if (!fake_file.open()) {
        fprintf(stderr, "%s\n", fake_file.getError().c_str());

        fake_file.close();

        return 1;
    }


    FFMPEG f;

    // ffmpeg init part 1
    if (!f.initFormat(fake_file)) {
        fprintf(stderr, "%s\n", f.getError().c_str());

        f.cleanup();
        fake_file.close();

        return 1;
    }


    // info printing
    if (cmd.info_wanted) {
        printInfo(f.fctx, fake_file);

        f.cleanup();
        fake_file.close();

        return 0;
    }


    // container format check
    if (getStreamType(f.fctx->iformat->name) == D2V::UNSUPPORTED_STREAM) {
        fprintf(stderr, "Unsupported container type '%s'.\n", f.fctx->iformat->long_name ? f.fctx->iformat->long_name : f.fctx->iformat->name);

        f.cleanup();
        fake_file.close();

        return 1;
    }


    // stream selection
    deselectAllStreams(f.fctx);

    AVStream *video_stream;
    if (cmd.have_video_id) {
        video_stream = selectVideoStreamById(f.fctx, cmd.video_id);
        if (!video_stream) {
            fprintf(stderr, "Couldn't find video track with id %x.\n", cmd.video_id);

            f.cleanup();
            fake_file.close();

            return 1;
        }
    } else {
        video_stream = selectFirstVideoStream(f.fctx);
        if (!video_stream) {
            fprintf(stderr, "Couldn't find any video tracks.\n");

            f.cleanup();
            fake_file.close();

            return 1;
        }
    }

    if (cmd.audio_ids.size()) {
        if (!selectAudioStreamsById(f.fctx, cmd.audio_ids)) {
            for (size_t i = 0; i < cmd.audio_ids.size(); i++)
                fprintf(stderr, "Couldn't find audio track with id %x.\n", cmd.audio_ids[i]);

            f.cleanup();
            fake_file.close();

            return 1;
        }
    } else if (cmd.audio_ids_all) {
        if (!selectAllAudioStreams(f.fctx)) {
            fprintf(stderr, "Couldn't find any audio tracks.\n");

            f.cleanup();
            fake_file.close();

            return 1;
        }
    }


    // video format check
    std::unordered_set<int> supported_codec_ids = {
        AV_CODEC_ID_MPEG1VIDEO,
        AV_CODEC_ID_MPEG2VIDEO
    };

    if (!supported_codec_ids.count(video_stream->codec->codec_id)) {
        const char *type = "unknown";
        const AVCodecDescriptor *desc = av_codec_get_codec_descriptor(video_stream->codec);
        if (desc)
            type = desc->long_name ? desc->long_name : desc->name;

        fprintf(stderr, "Unsupported video codec: %s (id: %d)\n", type, video_stream->codec->codec_id);

        f.cleanup();
        fake_file.close();

        return 1;
    }


    // d2v file opening
    FILE *d2v_file;
    if (cmd.d2v_path == "-") {
        d2v_file = stdout;
    } else {
        if (!cmd.d2v_path.size())
            cmd.d2v_path = fake_file[0].name + ".d2v";

        d2v_file = openFile(cmd.d2v_path.c_str(), "wb");
        if (!d2v_file) {
            fprintf(stderr, "Failed to open d2v file '%s' for writing: %s\n", cmd.d2v_path.c_str(), strerror(errno));

            f.cleanup();
            fake_file.close();

            return 1;
        }
    }


    // audio files opening
    std::unordered_map<int, FILE *> audio_files;
    for (unsigned i = 0; i < f.fctx->nb_streams; i++) {
        if (f.fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            f.fctx->streams[i]->discard != AVDISCARD_ALL) {
            std::string path = cmd.d2v_path;

            char id[20] = { 0 };
            snprintf(id, 19, "%x", f.fctx->streams[i]->id);

            path += " T";
            path += id;

            int64_t bit_rate, channel_layout;

            if (av_opt_get_int(f.fctx->streams[i]->codec, "channel_layout", 0, &channel_layout) >= 0) {
                char channels[512] = { 0 };
                av_get_channel_layout_string(channels, 512, 0, channel_layout);
                path += " ";
                path += channels;
            }

            if (av_opt_get_int(f.fctx->streams[i]->codec, "ab", 0, &bit_rate) >= 0)
                path += " " + std::to_string(bit_rate / 1000) + " kbps";

            path += ".audio";

            FILE *file = openFile(path.c_str(), "wb");
            if (!file) {
                fprintf(stderr, "Failed to open audio file '%s' for writing: %s\n", path.c_str(), strerror(errno));

                for (auto it = audio_files.begin(); it != audio_files.end(); it++)
                    fclose(it->second);
                f.cleanup();
                fake_file.close();

                return 1;
            }

            audio_files.insert({ f.fctx->streams[i]->index, file });
        }
    }


    // engage
    D2V::ProgressFunction progress_func = printProgress;
    D2V::LoggingFunction logging_func = printWarnings;
    if (cmd.stay_quiet) {
        progress_func = nullptr;
        logging_func = nullptr;
    }

    D2V d2v(d2v_file, audio_files, &fake_file, &f, video_stream, progress_func, logging_func);

    if (!d2v.engage()) {
        fprintf(stderr, "%s\n", d2v.getError().c_str());

        for (auto it = audio_files.begin(); it != audio_files.end(); it++)
            fclose(it->second);
        f.cleanup();
        fake_file.close();

        return 1;
    }

    if (!cmd.stay_quiet) {
        const D2V::Stats &stats = d2v.getStats();
        fprintf(stderr,
                "Video frames seen:   %d\n"
                "    Progressive:     %d\n"
                "    Top field first: %d\n"
                "    Repeat:          %d\n",
                stats.video_frames,
                stats.progressive_frames,
                stats.tff_frames,
                stats.rff_frames);
    }


    // some cleanup
    for (auto it = audio_files.begin(); it != audio_files.end(); it++)
        fclose(it->second);
    fclose(d2v_file);
    f.cleanup();
    fake_file.close();

    return 0;
}
