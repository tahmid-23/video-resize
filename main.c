#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

char errbuf[AV_ERROR_MAX_STRING_SIZE];

void print_error(const char *description, int errnum) {
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    printf("%s: %s\n", description, errbuf);
}

int write_packet(AVFormatContext *outfc, AVPacket *packet, AVStream *in_stream, AVStream *out_stream,
                 int out_stream_index) {
    packet->stream_index = out_stream_index;
    av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);

    int reason = av_interleaved_write_frame(outfc, packet);
    if (reason) {
        print_error("Failed to write frame", reason);
        return -1;
    }

    return 0;
}

int encode_frame_and_send(AVFormatContext *outfc, AVCodecContext *outcc, AVPacket *packet, AVFrame *frame,
                          AVStream *in_stream, AVStream *out_stream, int out_stream_index) {
    int reason = avcodec_send_frame(outcc, frame);
    if (reason) {
        print_error("Failed to send encode frame", reason);
        return -1;
    }

    int packet_reason;
    for (packet_reason = avcodec_receive_packet(outcc, packet);
         packet_reason >= 0; packet_reason = avcodec_receive_packet(outcc, packet)) {
        reason = write_packet(outfc, packet, in_stream, out_stream, out_stream_index);
        if (reason) {
            return reason;
        }

        av_packet_unref(packet);
    }

    if (packet_reason != AVERROR_EOF && packet_reason != AVERROR(EAGAIN)) {
        print_error("Failed to receive encode packets", reason);
        return -1;
    }

    return 0;
}

int transcode(AVFormatContext *outfc, AVCodecContext *incc, AVCodecContext *outcc, AVPacket *packet, AVFrame *frame,
              AVStream *in_stream, AVStream *out_stream, int out_stream_index) {
    int reason = avcodec_send_packet(incc, packet);
    if (reason) {
        print_error("Failed to send decode packet", reason);
        return -1;
    }

    av_packet_unref(packet);
    int frame_reason;
    for (frame_reason = avcodec_receive_frame(incc, frame);
         frame_reason >= 0; frame_reason = avcodec_receive_frame(incc, frame)) {
        encode_frame_and_send(outfc, outcc, packet, frame, in_stream, out_stream, out_stream_index);
        av_frame_unref(frame);
    }

    if (frame_reason != AVERROR_EOF && frame_reason != AVERROR(EAGAIN)) {
        print_error("Failed to receive decode frames", reason);
        return -1;
    }

    return 0;
}

int write_body(AVFormatContext *infc, AVFormatContext *outfc, const int *out_stream_indices,
               AVCodecContext **in_codec_contexts, AVCodecContext **out_codec_contexts) {
    int result = 0;

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    while (av_read_frame(infc, packet) >= 0) {
        if (out_stream_indices[packet->stream_index] == -1) {
            av_packet_unref(packet);
            continue;
        }

        AVStream *in_stream = infc->streams[packet->stream_index];
        int out_stream_index = out_stream_indices[packet->stream_index];
        AVStream *out_stream = outfc->streams[out_stream_index];

        AVCodecContext *outcc = out_codec_contexts[packet->stream_index];
        if (outcc) {
            int reason = transcode(outfc, in_codec_contexts[packet->stream_index], outcc, packet, frame, in_stream,
                                   out_stream,
                                   out_stream_index);
            if (reason) {
                result = reason;
                goto end;
            }
        } else {
            int reason = write_packet(outfc, packet, in_stream, out_stream, out_stream_index);
            if (reason) {
                result = reason;
                goto end;
            }

            av_packet_unref(packet);
        }
    }

    end:
    av_frame_free(&frame);
    av_packet_free(&packet);
    return result;
}

int write_output(AVFormatContext *infc, AVFormatContext *outfc, const int *out_stream_indices,
                 AVCodecContext **in_codec_contexts, AVCodecContext **out_codec_contexts) {
    int reason = avformat_write_header(outfc, NULL);
    if (reason != AVSTREAM_INIT_IN_WRITE_HEADER) {
        print_error("Failed to write header to output file", reason);
        return -1;
    }

    reason = write_body(infc, outfc, out_stream_indices, in_codec_contexts, out_codec_contexts);
    if (reason) {
        return reason;
    }

    reason = av_write_trailer(outfc);
    if (reason) {
        print_error("Failed to write trailer", reason);
        return -1;
    }

    return 0;
}

AVCodecContext *create_decode_context(const AVCodec *in_codec, AVCodecParameters *in_parameters) {
    AVCodecContext *incc = avcodec_alloc_context3(in_codec);
    if (!incc) {
        printf("Failed to allocate memory for input in_stream codec context\n");
        return NULL;
    }

    int reason = avcodec_parameters_to_context(incc, in_parameters);
    if (reason) {
        print_error("Failed to copy parameters to context", reason);
        avcodec_free_context(&incc);
        return NULL;
    }

    reason = avcodec_open2(incc, in_codec, NULL);
    if (reason) {
        print_error("Failed to open input codec context", reason);
        avcodec_free_context(&incc);
        return NULL;
    }

    return incc;
}

AVCodecContext *
create_encode_context(const AVCodec *out_codec, AVCodecContext *incc, AVFormatContext *infc, AVStream *in_stream) {
    AVCodecContext *outcc = avcodec_alloc_context3(out_codec);
    if (!outcc) {
        printf("Failed to allocate memory for output in_stream codec context\n");
        return NULL;
    }

    outcc->width = incc->width;
    outcc->height = incc->height;
    outcc->sample_aspect_ratio = incc->sample_aspect_ratio;
    outcc->pix_fmt = incc->pix_fmt;
    outcc->bit_rate = incc->bit_rate;

    AVRational frame_rate = av_guess_frame_rate(infc, in_stream, NULL);
    outcc->time_base = av_inv_q(frame_rate);

    int reason = avcodec_open2(outcc, out_codec, NULL);
    if (reason) {
        print_error("Failed to open output codec context", reason);
        avcodec_free_context(&outcc);
        return NULL;
    }

    return outcc;
}

int create_streams(AVFormatContext *infc, AVFormatContext *outfc, int *out_stream_indices, const AVCodec *out_codec,
                   AVCodecContext **in_codec_contexts, AVCodecContext **out_codec_contexts) {
    int stream_count = -1;
    for (int i = 0; i < infc->nb_streams; ++i) {
        AVStream *in_stream = infc->streams[i];
        AVCodecParameters *in_parameters = in_stream->codecpar;
        if (in_parameters->codec_type != AVMEDIA_TYPE_AUDIO && in_parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
            out_stream_indices[i] = -1;
            continue;
        }

        AVCodecContext *outcc;
        if (in_parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            const AVCodec *in_codec = avcodec_find_decoder(in_parameters->codec_id);
            if (!in_codec) {
                printf("Failed to find decoder\n");
                goto failure;
            }

            AVCodecContext *incc = create_decode_context(in_codec, in_parameters);
            if (!incc) {
                goto failure;
            }
            in_codec_contexts[i] = incc;

            outcc = create_encode_context(out_codec, incc, infc, in_stream);
            if (!outcc) {
                goto failure;
            }
            out_codec_contexts[i] = outcc;
        }

        AVStream *out_stream = avformat_new_stream(outfc, NULL);
        if (!out_stream) {
            printf("Failed to create output in_stream\n");
            goto failure;
        }

        if (in_parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            int reason = avcodec_parameters_from_context(out_stream->codecpar, outcc);
            if (reason) {
                print_error("Failed to copy codec parameters from codec context to output in_stream\n", reason);
                goto failure;
            }

            out_stream->time_base = outcc->time_base;
        } else {
            int reason = avcodec_parameters_copy(out_stream->codecpar, in_parameters);
            if (reason) {
                print_error("Failed to copy codec parameters to output in_stream\n", reason);
                goto failure;
            }
        }

        out_stream_indices[i] = ++stream_count;
    }

    return 0;

    failure:
    for (int i = 0; i < infc->nb_streams; ++i) {
        avcodec_free_context(&in_codec_contexts[i]);
        avcodec_free_context(&out_codec_contexts[i]);
    }
    return -1;
}

int create_streams_and_transcode(AVFormatContext *infc, AVFormatContext *outfc, char *output_file) {
    const AVCodec *out_codec = avcodec_find_encoder_by_name("libx265");
    if (!out_codec) {
        printf("Failed to find libx265 codec\n");
        return -1;
    }

    int *out_stream_indices = av_malloc_array(infc->nb_streams, sizeof *out_stream_indices);
    if (!out_stream_indices) {
        printf("Failed to allocate memory for output stream indices\n");
        return -1;
    }

    AVCodecContext **in_codec_contexts = av_calloc(infc->nb_streams, sizeof(AVCodecContext *));
    if (!in_codec_contexts) {
        printf("Failed to allocate memory for input codec contexts\n");
        av_freep(&out_stream_indices);
        return -1;
    }

    AVCodecContext **out_codec_contexts = av_calloc(infc->nb_streams, sizeof(AVCodecContext *));
    if (!out_codec_contexts) {
        printf("Failed to allocate memory for output codec contexts\n");
        av_freep(&in_codec_contexts);
        av_freep(&out_stream_indices);
        return -1;
    }

    if (create_streams(infc, outfc, out_stream_indices, out_codec, in_codec_contexts, out_codec_contexts)) {
        av_freep(&out_codec_contexts);
        av_freep(&in_codec_contexts);
        av_freep(&out_stream_indices);
        return -1;
    }

    int reason = avio_open(&outfc->pb, output_file, AVIO_FLAG_WRITE);
    if (reason) {
        print_error("Failed to open output file", reason);

        for (int i = 0; i < infc->nb_streams; ++i) {
            avcodec_free_context(&in_codec_contexts[i]);
            avcodec_free_context(&out_codec_contexts[i]);
        }
        av_freep(&out_codec_contexts);
        av_freep(&in_codec_contexts);
        av_freep(&out_stream_indices);
        return -1;
    }

    int result = write_output(infc, outfc, out_stream_indices, in_codec_contexts, out_codec_contexts);
    avio_closep(&outfc->pb);
    for (int i = 0; i < infc->nb_streams; ++i) {
        avcodec_free_context(&in_codec_contexts[i]);
        avcodec_free_context(&out_codec_contexts[i]);
    }
    av_freep(&out_codec_contexts);
    av_freep(&in_codec_contexts);
    av_freep(&out_stream_indices);
    return result;
}

int main() {
    char buf[256];

    char input_file[256];
    printf("Enter an input file: ");
    fgets(buf, 256, stdin);
    sscanf(buf, "%[^\n]", input_file);

    char output_file[256];
    printf("Enter an output file: ");
    fgets(buf, 256, stdin);
    sscanf(buf, "%[^\n]", output_file);

    AVFormatContext *infc = avformat_alloc_context();
    if (!infc) {
        printf("Failed to allocate memory for input format context\n");
        return -1;
    }

    int reason = avformat_open_input(&infc, input_file, NULL, NULL);
    if (reason) {
        print_error("Failed to open input file", reason);
        avformat_free_context(infc);
        return -1;
    }

    reason = avformat_find_stream_info(infc, NULL);
    if (reason) {
        print_error("Failed to query stream info", reason);
        avformat_close_input(&infc);
        return -1;
    }

    AVFormatContext *outfc;
    reason = avformat_alloc_output_context2(&outfc, NULL, "mp4", NULL);
    if (reason) {
        print_error("Failed to create output context", reason);
        avformat_close_input(&infc);
        return -1;
    }

    int result = create_streams_and_transcode(infc, outfc, output_file);
    avformat_free_context(outfc);
    avformat_close_input(&infc);
    return result;
}
