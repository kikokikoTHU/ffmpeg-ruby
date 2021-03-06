/* Copyright (c)2008 Antonin Amand.
 * Licensed under the Ruby License. See LICENSE for details.
 *
 */
#include "ffmpeg.h"
#include "ffmpeg_utils.h"

VALUE rb_cFFMPEGInputFormat;
VALUE rb_eUnsupportedFormat;

static VALUE
input_format_initialize(VALUE self, VALUE filename)
{
    AVFormatContext * format_context = NULL;

    Data_Get_Struct(self, AVFormatContext, format_context);
    if (Qfalse == rb_funcall(rb_cFile, rb_intern("file?"), 1, filename))
        rb_raise(rb_eArgError,
            "ffmpeg failed to open input file %s",
            StringValuePtr(filename));

    int error = avformat_open_input(&format_context, StringValuePtr(filename),
        NULL, NULL);

    if (error < 0) {
        DATA_PTR(self) = format_context;
        rb_raise(rb_eUnsupportedFormat,
            "ffmpeg failed to open input file %s",
            StringValuePtr(filename));
    }

    error = avformat_find_stream_info(format_context, NULL);

    if (error < 0) {
        DATA_PTR(self) = format_context;
        rb_raise(rb_eUnsupportedFormat,
            "ffmpeg internal error while looking for stream info on %s",
            StringValuePtr(filename));
    }

    return self;
}

static VALUE
input_format_available_input_formats(VALUE klass)
{
    return rb_cv_get(klass, "@@available_input_formats");
}

void Init_FFMPEGInputFormat() {
    rb_cFFMPEGFormat = rb_const_get(rb_mFFMPEG, rb_intern("Format"));
    rb_cFFMPEGInputFormat = rb_define_class_under(rb_mFFMPEG, "InputFormat", rb_cFFMPEGFormat);
    rb_define_method(rb_cFFMPEGInputFormat, "initialize", input_format_initialize, 1);
    // rb_define_method(rb_cFFMPEGInputFormat, "seek", input_format_seek, 1);
    // rb_define_method(rb_cFFMPEGInputFormat, "position", input_format_position, 0);
    // rb_define_method(rb_cFFMPEGInputFormat, "extract_next_frame", input_format_extract_next_frame, 1);

    rb_eUnsupportedFormat = rb_define_class_under(rb_mFFMPEG, "UnsupportedFormat", rb_eStandardError);
}
