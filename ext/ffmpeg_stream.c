/* Copyright (c)2008 Antonin Amand.
 * Licensed under the Ruby License. See LICENSE for details.
 *
 */
#include "ffmpeg.h"
#include "ffmpeg_utils.h"
#include <unistd.h>

VALUE rb_cFFMPEGStream;
int audio_count = 0;
int video_count = 0;

static int
next_packet(AVFormatContext * format_context, AVPacket * packet)
{
    if(packet->data != NULL)
    {
        av_free_packet(packet);
    }

    int ret = av_read_frame(format_context, packet);
    if(ret < 0)
        return -1;

    return 0;
}

static int
next_packet_for_stream(AVFormatContext * format_context, int stream_index, AVPacket * packet)
{
    int ret = 0;
    do {
        ret = next_packet(format_context, packet);
    } while(packet->stream_index != stream_index && ret == 0);

    return ret;
}

static VALUE
stream_codec(VALUE self)
{
    AVStream * stream = get_stream(self);

    VALUE rb_codec = rb_iv_get(self, "@codec");

    if (rb_codec == Qnil && NULL != stream->codec)
        rb_codec = rb_iv_set(self, "@codec", build_codec_object(stream->codec));

    return rb_codec;
}

static VALUE
stream_index(VALUE self)
{
    AVStream * stream = get_stream(self);
    return INT2FIX(stream->index);
}

static VALUE
stream_duration(VALUE self)
{
    AVStream * stream = get_stream(self);
    if (stream->duration == AV_NOPTS_VALUE) {
        return Qnil;
    }
    return(rb_float_new(stream->duration * av_q2d(stream->time_base)));
}

static VALUE
stream_get_rotation(VALUE self)
{
    AVStream *stream = get_stream(self);
    if (!stream || !stream->metadata)
        return Qnil;
    AVDictionaryEntry *rotate_tag = av_dict_get(stream->metadata, "rotate", NULL, 0);
    if(rotate_tag)
        return rb_str_new(rotate_tag->value, strlen(rotate_tag->value));
    return Qnil;
}
static VALUE
stream_time_base(VALUE self)
{
    AVStream * stream = get_stream(self);
    return(rb_float_new(av_q2d(stream->time_base)));
}

static VALUE
stream_frame_count(VALUE self)
{
    AVStream * stream = get_stream(self);
    return(rb_float_new(stream->nb_frames));
}

static VALUE
stream_frame_rate(VALUE self)
{
    AVStream * stream = get_stream(self);
    return(rb_float_new(av_q2d(stream->r_frame_rate)));
}

static VALUE
stream_seek(VALUE self, VALUE position)
{
    AVFormatContext * format_context = get_format_context(rb_iv_get(self, "@format"));
    AVStream * stream = get_stream(self);
    int64_t timestamp = NUM2LONG(position) / av_q2d(stream->time_base);

    int ret;
    if (format_context->start_time != AV_NOPTS_VALUE)
        timestamp += format_context->start_time;

    ret = av_seek_frame(format_context, stream->index, timestamp, AVSEEK_FLAG_ANY);
    if (ret < 0) {
        rb_raise(rb_eRangeError, "could not seek %s to pos %f",
            format_context->filename, timestamp * av_q2d(stream->time_base));
    }

    return self;
}

static VALUE
stream_seek_by_frame(VALUE self, VALUE position)
{
    AVFormatContext * format_context = get_format_context(rb_iv_get(self, "@format"));
    AVStream * stream = get_stream(self);

    int64_t timestamp = NUM2LONG(position) * AV_TIME_BASE;

    int ret;
    if (format_context->start_time != AV_NOPTS_VALUE)
       timestamp += format_context->start_time;

    ret = av_seek_frame(format_context, stream->index, timestamp, AVSEEK_FLAG_ANY);
    if (ret < 0) {
       rb_raise(rb_eRangeError, "could not seek %s to pos %f",
           format_context->filename, timestamp * av_q2d(stream->time_base));
    }

    return self;
}

static VALUE
stream_position(VALUE self)
{
    AVFormatContext * format_context = get_format_context(rb_iv_get(self, "@format"));
    AVStream * stream = get_stream(self);
    AVPacket decoding_packet;

    av_init_packet(&decoding_packet);

    do {
        if(av_read_frame(format_context, &decoding_packet) < 0) {
            rb_raise(rb_eRuntimeError, "error extracting packet");
        }
    } while(decoding_packet.stream_index != stream->index);

    printf("%f %f c-pos:%f\n", decoding_packet.pts, (double)av_q2d(stream->time_base),
        decoding_packet.pts * (double)av_q2d(stream->time_base));

    return rb_float_new(decoding_packet.pts * (double)av_q2d(stream->time_base));
}

// src_buf: origin audio data
// dst_buf: resample audio data
// src_c:   origin channel
// dst_c:   resample channel
// src_s:   origin sample rate
// dst_s:   resample sample rate
// src_size:origin audio data buffer size
// dst_size:resample audio data buffer size
static int
stream_resample(char *src_buf, char *dst_buf, int src_c,
    int dst_c, int src_s, int dst_s, int src_size)
{
    SwrContext *swr = swr_alloc();
    av_opt_set_int(swr, "in_channel_layout",  src_c, 0);
    av_opt_set_int(swr, "out_channel_layout", dst_c,  0);
    av_opt_set_int(swr, "in_sample_rate",     src_s, 0);
    av_opt_set_int(swr, "out_sample_rate",    dst_s, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);
    swr_init(swr);

    int src_nb_samples = src_size/4;
    int dst_nb_samples = av_rescale_rnd(src_nb_samples, dst_s, src_s, AV_ROUND_UP);
    int sample_size = swr_convert(swr, &dst_buf, dst_nb_samples, &src_buf, src_nb_samples);
    sample_size *= 2;

    if (swr)
        swr_free(&swr);
    return sample_size;
}

static int
extract_next_frame(AVFormatContext * format_context, AVCodecContext * codec_context,
    int stream_index, AVFrame * frame, AVPacket * decoding_packet)
{
    if (NULL == codec_context->codec) {
            rb_fatal("codec should have already been opened");
    }

    uint8_t * databuffer;
    int remaining = 0;
    int decoded;
    int frame_complete = 0;
    int next;

    while(!frame_complete &&
            0 == (next = next_packet_for_stream(format_context, stream_index, decoding_packet))) {
        // fprintf(stderr, "frame complete?: %d\n", frame_complete);
        remaining = decoding_packet->size;
        databuffer = decoding_packet->data;
        while(remaining > 0) {
            decoded = avcodec_decode_video2(codec_context, frame,
                &frame_complete, decoding_packet);
            remaining -= decoded;
            databuffer += decoded;
        }
    }

    return next;
}

static int
extract_next_audio(AVFormatContext * format_context, AVCodecContext * codec_context,
    int stream_index, uint8_t **raw_data, AVPacket * decoding_packet)
{
    if (NULL == codec_context->codec) {
            rb_fatal("codec should have already been opened");
    }

    uint8_t * databuffer;
    int remaining = 0;
    int decoded;
    int frame_complete = 0;
    int next;
    int buf_cap = 192000;
    int buf_size = 0;
    *raw_data = malloc(buf_cap);

    while(1) {
        int flag = 0;
        frame_complete = 0;
        while(!frame_complete &&
                0 == (next = next_packet_for_stream(format_context, stream_index, decoding_packet))) {

            remaining = decoding_packet->size;
            databuffer = decoding_packet->data;

            while(remaining > 0) {
                AVFrame *decoding_frame = NULL;
                if (!decoding_frame)
                    if (!(decoding_frame=av_frame_alloc()))
                        rb_raise(rb_eRuntimeError, "error allocate memory for decode audio.");
                decoded = avcodec_decode_audio4(codec_context, decoding_frame,
                            &frame_complete, decoding_packet);
                if (frame_complete) {
                    int out_linesize;
                    int out_size = av_samples_get_buffer_size(&out_linesize, codec_context->channels,
                                    decoding_frame->nb_samples, codec_context->sample_fmt, 1);

                    if ((buf_size+out_size)>=buf_cap){
                        buf_cap *= 2;
                        uint8_t *tmp = malloc(buf_cap);
                        memcpy(tmp, *raw_data, buf_size);
                        free(*raw_data);
                        *raw_data = tmp;
                        tmp = NULL;
                    }

                    memcpy(*raw_data+buf_size, decoding_frame->data[0], out_size);
                    buf_size += out_size;
                    flag += out_size;
                }
                remaining -= decoded;
                databuffer += decoded;
                av_frame_free(&decoding_frame);
            }
        }
        if(flag <= 0)
            break;
    }

    av_packet_unref(decoding_packet);
    decoding_packet->data = NULL;
    decoding_packet->size = 0;
    frame_complete = 0;
    do {
       if (decoding_packet->stream_index == stream_index) {
          AVFrame *decoding_frame = NULL;
          decoded = avcodec_decode_audio4(codec_context, decoding_frame,
            &frame_complete, decoding_packet);
          if (frame_complete) {
              int out_linesize;
              int out_size = av_samples_get_buffer_size(&out_linesize, codec_context->channels,
                              decoding_frame->nb_samples, codec_context->sample_fmt, 1);

              if ((buf_size+out_size)>=buf_cap){
                  buf_cap *= 2;
                  uint8_t *tmp = malloc(buf_cap);
                  memcpy(tmp, *raw_data, buf_size);
                  free(*raw_data);
                  *raw_data = tmp;
                  tmp = NULL;
              }

              memcpy(*raw_data+buf_size, decoding_frame->data[0], out_size);
              buf_size += out_size;
          }
          fprintf(stderr, "[%d, %d]\n", decoded, frame_complete);
          av_frame_free(&decoding_frame);
       }
    } while (frame_complete);
    return buf_size;
}

static VALUE
stream_decode_audio(VALUE self, VALUE rb_channel, VALUE rb_sample_rate)
{

    AVFormatContext * format_context = get_format_context(rb_iv_get(self, "@format"));
    AVStream * stream = get_stream(self);

    AVCodecContext * codec_context = stream->codec;

    if (!codec_context->codec) {
        AVCodec * codec = avcodec_find_decoder(codec_context->codec_id);
        if (!codec)
            rb_raise(rb_eRuntimeError, "error codec not found");
        if (avcodec_open2(codec_context, codec, NULL) < 0)
            rb_raise(rb_eRuntimeError, "error while opening codec : %s", codec->name);
    }

    AVPacket decoding_packet;
    av_init_packet(&decoding_packet);

    char *raw_data;
    VALUE audio_stream = Qnil;
    int size = extract_next_audio(format_context, codec_context, stream->index, &raw_data, &decoding_packet);
    if (size > 0) {
        int channel = FIX2INT(rb_channel);
        int sample_rate = FIX2INT(rb_sample_rate);
        if (channel || sample_rate) {
            int src_chan = codec_context->channel_layout;
            int src_rate = codec_context->sample_rate;

            channel = (channel>=0 ? channel : src_chan);
            sample_rate = (sample_rate ? sample_rate : src_rate);

            int init_sample_size = av_rescale_rnd(size, sample_rate, src_rate, AV_ROUND_UP);
            char *resample_data = malloc(init_sample_size);
            int resample_size = stream_resample(raw_data, resample_data, src_chan, channel,
                                src_rate, sample_rate, size);
            if (resample_size > 0) {
                free(raw_data);
                raw_data = resample_data;
                resample_data = NULL;
                size = resample_size;
            }
        }
        audio_stream = rb_str_new(raw_data, size);
        free(raw_data);
        raw_data = NULL;
    }


    av_free_packet(&decoding_packet);
    return audio_stream;
}

static VALUE
stream_decode_frame(VALUE self)
{
    AVFormatContext * format_context = get_format_context(rb_iv_get(self, "@format"));
    AVStream * stream = get_stream(self);

    AVCodecContext * codec_context = stream->codec;

    // open codec to decode the video if needed
    if (!codec_context->codec) {
        AVCodec * codec = avcodec_find_decoder(codec_context->codec_id);
        if (!codec)
            rb_raise(rb_eRuntimeError, "error codec not found");
        if (avcodec_open2(codec_context, codec, NULL) < 0)
            rb_raise(rb_eRuntimeError, "error while opening codec : %s", codec->name);
    }

    VALUE rb_frame = rb_funcall(rb_const_get(rb_mFFMPEG, rb_intern("Frame")),
        rb_intern("new"), 4,
        INT2NUM(codec_context->width),
        INT2NUM(codec_context->height),
        INT2NUM(0),
        INT2NUM(codec_context->pix_fmt));

    AVFrame * frame = get_frame(rb_frame);

    AVPacket decoding_packet;
    av_init_packet(&decoding_packet);

    if (rb_block_given_p()) {
        int ret;
        do {
            ret = extract_next_frame(format_context, stream->codec,
                stream->index, frame, &decoding_packet);
            rb_yield(
                rb_ary_new3(
                    3,
                    rb_frame,
                    rb_float_new(decoding_packet.pts * (double)av_q2d(stream->time_base)),
                    rb_float_new(decoding_packet.dts * (double)av_q2d(stream->time_base))
                )
            );
        } while (ret == 0);

        av_packet_unref(&decoding_packet);
        decoding_packet.data = NULL;
        decoding_packet.size = 0;

        int frame_complete;
        do {
           if (decoding_packet.stream_index == stream->index) {
              ret = avcodec_decode_video2(stream->codec, frame,
                &frame_complete, &decoding_packet);
              if (frame_complete) {
                  rb_yield(
                      rb_ary_new3(
                          3,
                          rb_frame,
                          rb_float_new(decoding_packet.pts * (double)av_q2d(stream->time_base)),
                          rb_float_new(decoding_packet.dts * (double)av_q2d(stream->time_base))
                      )
                  );
              }
           }
        } while (frame_complete);
    } else
      rb_raise(rb_eRuntimeError, "no block given");

    av_free_packet(&decoding_packet);
    return self;
}


// ######################  CONSTRUCT / DESTROY #############################

void
mark_stream(AVStream * stream)
{}

void
free_stream(AVStream * stream)
{
}

static VALUE
alloc_stream(VALUE klass)
{
    AVStream * stream = av_new_stream(NULL, 0);
    return Data_Wrap_Struct(rb_cFFMPEGStream, 0, 0, stream);
}

static VALUE
stream_initialize(VALUE self, VALUE format)
{
    rb_iv_set(self, "@format", format);
    return self;
}

VALUE build_stream_object(AVStream * stream, VALUE rb_format)
{
    VALUE rb_stream = Data_Wrap_Struct(rb_cFFMPEGStream, 0, 0, stream);
    return stream_initialize(rb_stream, rb_format);
}

void
Init_FFMPEGStream()
{
    rb_cFFMPEGStream = rb_define_class_under(rb_mFFMPEG, "Stream", rb_cObject);
    rb_define_alloc_func(rb_cFFMPEGStream, alloc_stream);
    rb_define_method(rb_cFFMPEGStream, "initialize", stream_initialize, 0);

    rb_define_method(rb_cFFMPEGStream, "index", stream_index, 0);
    rb_define_method(rb_cFFMPEGStream, "codec", stream_codec, 0);
    rb_define_method(rb_cFFMPEGStream, "duration", stream_duration, 0);
    rb_define_method(rb_cFFMPEGStream, "time_base", stream_time_base, 0);
    rb_define_method(rb_cFFMPEGStream, "frame_count", stream_frame_count, 0);
    rb_define_method(rb_cFFMPEGStream, "frame_rate", stream_frame_rate, 0);
    rb_define_method(rb_cFFMPEGStream, "position", stream_position, 0);
    rb_define_method(rb_cFFMPEGStream, "get_rotation", stream_get_rotation, 0);
    rb_define_method(rb_cFFMPEGStream, "decode_frame", stream_decode_frame, 0);
    rb_define_method(rb_cFFMPEGStream, "decode_audio", stream_decode_audio, 2);
    rb_define_method(rb_cFFMPEGStream, "seek", stream_seek, 1);
    rb_define_method(rb_cFFMPEGStream, "seek_by_frame", stream_seek_by_frame, 1);
}
