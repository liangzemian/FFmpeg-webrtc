/*
 * Multipart JPEG format
 * Copyright (c) 2015 Luca Barbato
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"



typedef struct MPJPEGDemuxContext {
    const AVClass *class;
    char       *boundary;
    char       *searchstr;
    int         searchstr_len;
    int         strict_mime_boundary;
} MPJPEGDemuxContext;


static void trim_right(char *p)
{
    char *end;

    if (!p || !*p)
        return;

    end = p + strlen(p);
    while (end > p && av_isspace(*(end-1)))
        *(--end) = '\0';
}

static int get_line(AVIOContext *pb, char *line, int line_size)
{
    ff_get_line(pb, line, line_size);

    if (pb->error)
        return pb->error;

    if (pb->eof_reached)
        return AVERROR_EOF;

    trim_right(line);
    return 0;
}



static int split_tag_value(char **tag, char **value, char *line)
{
    char *p = line;
    int  foundData = 0;

    *tag = NULL;
    *value = NULL;


    while (*p != '\0' && *p != ':') {
        if (!av_isspace(*p)) {
            foundData = 1;
        }
        p++;
    }
    if (*p != ':')
        return foundData ? AVERROR_INVALIDDATA : 0;

    *p   = '\0';
    *tag = line;
    trim_right(*tag);

    p++;

    while (av_isspace(*p))
        p++;

    *value = p;
    trim_right(*value);

    return 0;
}

static int parse_multipart_header(AVIOContext *pb,
                                    int* size,
                                    const char* expected_boundary,
                                    void *log_ctx,
                                    int* pts);

static int mpjpeg_read_close(AVFormatContext *s)
{
    MPJPEGDemuxContext *mpjpeg = s->priv_data;
    av_freep(&mpjpeg->boundary);
    av_freep(&mpjpeg->searchstr);
    return 0;
}

static int mpjpeg_read_probe(AVProbeData *p)
{
    AVIOContext *pb;
    int ret = 0;
    int size = 0;
    int pts = 0;

    if (p->buf_size < 2 || p->buf[0] != '-' || p->buf[1] != '-')
        return 0;

    pb = avio_alloc_context(p->buf, p->buf_size, 0, NULL, NULL, NULL, NULL);
    if (!pb)
        return 0;

    ret = (parse_multipart_header(pb, &size, "--", NULL, &pts) >= 0) ? AVPROBE_SCORE_MAX : 0;

    avio_context_free(&pb);

    return ret;
}

static int mpjpeg_read_header(AVFormatContext *s)
{
    AVStream *st;
    char boundary[70 + 2 + 1] = {0};
    int64_t pos = avio_tell(s->pb);
    int ret;

    do {
        ret = get_line(s->pb, boundary, sizeof(boundary));
        if (ret < 0)
            return ret;
    } while (!boundary[0]);

    if (strncmp(boundary, "--", 2))
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_MJPEG;

    avpriv_set_pts_info(st, 60, 1, 25);

    avio_seek(s->pb, pos, SEEK_SET);

    return 0;
}

static int parse_content_length(const char *value)
{
    long int val = strtol(value, NULL, 10);

    if (val == LONG_MIN || val == LONG_MAX)
        return AVERROR(errno);
    if (val > INT_MAX)
        return AVERROR(ERANGE);
    return val;
}

static int parse_multipart_header(AVIOContext *pb,
                            int* size,
                            const char* expected_boundary,
                            void *log_ctx,
                            int* pts)
{
    char line[128];
    int found_content_type = 0;
    int ret;

    *size = -1;

    // get the CRLF as empty string
    ret = get_line(pb, line, sizeof(line));
    if (ret < 0)
        return ret;

    /* some implementation do not provide the required
     * initial CRLF (see rfc1341 7.2.1)
     */
    while (!line[0]) {
        ret = get_line(pb, line, sizeof(line));
        if (ret < 0)
            return ret;
    }

    if (!av_strstart(line, expected_boundary, NULL)) {
        if (log_ctx)
        av_log(log_ctx,
            AV_LOG_ERROR,
            "Expected boundary '%s' not found, instead found a line of %"SIZE_SPECIFIER" bytes\n",
            expected_boundary,
            strlen(line));

        return AVERROR_INVALIDDATA;
    }

    while (!pb->eof_reached) {
        char *tag, *value;

        ret = get_line(pb, line, sizeof(line));
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                break;
            return ret;
        }

        if (line[0] == '\0')
            break;

        ret = split_tag_value(&tag, &value, line);
        if (ret < 0)
            return ret;
        if (value==NULL || tag==NULL)
            break;

        if (log_ctx)
            av_log(log_ctx, AV_LOG_DEBUG,"mpjpegdec.cccccc %s : %s\n", tag, value);

        if (!av_strcasecmp(tag, "Content-type")) {
            if (av_strcasecmp(value, "image/jpeg")) {
                if (log_ctx)
                av_log(log_ctx, AV_LOG_ERROR,
                           "Unexpected %s : %s\n",
                           tag, value);
                return AVERROR_INVALIDDATA;
            } else
                found_content_type = 1;
        } else if (!av_strcasecmp(tag, "Content-Length")) {
            *size = parse_content_length(value);
            if ( *size < 0 )
                if (log_ctx)
                av_log(log_ctx, AV_LOG_WARNING,
                           "Invalid Content-Length value : %s\n",
                           value);
        } else if (!av_strcasecmp(tag, "X-Timestamp")) {
            *pts = parse_content_length(value);
        }
    }

    return found_content_type ? 0 : AVERROR_INVALIDDATA;
}


static char* mpjpeg_get_boundary(AVIOContext* pb)
{
    uint8_t *mime_type = NULL;
    const char *start;
    const char *end;
    uint8_t *res = NULL;
    int     len;

    /* get MIME type, and skip to the first parameter */
    av_opt_get(pb, "mime_type", AV_OPT_SEARCH_CHILDREN, &mime_type);
    start = mime_type;
    while (start != NULL && *start != '\0') {
        start = strchr(start, ';');
        if (!start)
            break;

        start = start+1;

        while (av_isspace(*start))
            start++;

        if (!av_stristart(start, "boundary=", &start)) {
            end = strchr(start, ';');
            if (end)
                len = end - start - 1;
            else
                len = strlen(start);

            /* some endpoints may enclose the boundary
              in Content-Type in quotes */
            if ( len>2 && *start == '"' && start[len-1] == '"' ) {
                start++;
                len -= 2;
            }
            res = av_strndup(start, len);
            break;
        }
    }

    av_freep(&mime_type);
    return res;
}


static int mpjpeg_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int size;
    int ret;
    int pts;

    MPJPEGDemuxContext *mpjpeg = s->priv_data;
    if (mpjpeg->boundary == NULL) {
        uint8_t* boundary = NULL;
        if (mpjpeg->strict_mime_boundary) {
            boundary = mpjpeg_get_boundary(s->pb);
        }
        if (boundary != NULL) {
            mpjpeg->boundary = boundary;
            mpjpeg->searchstr = av_asprintf( "\r\n%s\r\n", boundary );
        } else {
            mpjpeg->boundary = av_strdup("--");
            mpjpeg->searchstr = av_strdup("\r\n--");
        }
        if (!mpjpeg->boundary || !mpjpeg->searchstr) {
            av_freep(&mpjpeg->boundary);
            av_freep(&mpjpeg->searchstr);
            return AVERROR(ENOMEM);
        }
        mpjpeg->searchstr_len = strlen(mpjpeg->searchstr);
    }

    ret = parse_multipart_header(s->pb, &size, mpjpeg->boundary, s, &pts);


    if (ret < 0)
        return ret;

    if (size > 0) {
        /* size has been provided to us in MIME header */
        ret = av_get_packet(s->pb, pkt, size);

        // 获取 packet-buffering 参数
        AVDictionaryEntry *entry = av_dict_get(s->metadata, "packet-buffering", NULL, 0);
        if (entry) {
            av_log(s, AV_LOG_INFO, "av_dict_set_int_packet-buffering222: %s\n", entry->value);
            int realtime = (int) strtol(entry->value, NULL, 10);
            if (realtime == 0)
                return ret;
        }

        AVRational src_tb = {1, 1000}; // 原始时间基
        AVRational dst_tb = {1, 30}; // 目标时间基
        pkt->pts = av_rescale_q(pts, src_tb, dst_tb);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(1, dst_tb, dst_tb);
        pkt->pos = -1;
    } else {
        /* no size was given -- we read until the next boundary or end-of-file */
        int remaining = 0, len;

        const int read_chunk = 2048;
        av_init_packet(pkt);
        pkt->data = NULL;
        pkt->size = 0;
        pkt->pos  = avio_tell(s->pb);
        pkt->pts = pts;
        /* we may need to return as much as all we've read back to the buffer */
        ffio_ensure_seekback(s->pb, read_chunk);

        while ((ret = av_append_packet(s->pb, pkt, read_chunk - remaining)) >= 0) {
            /* scan the new data */
            char *start;

            len = ret + remaining;
            start = pkt->data + pkt->size - len;
            do {
                if (!memcmp(start, mpjpeg->searchstr, mpjpeg->searchstr_len)) {
                    // got the boundary! rewind the stream
                    avio_seek(s->pb, -len, SEEK_CUR);
                    pkt->size -= len;
                    return pkt->size;
                }
                len--;
                start++;
            } while (len >= mpjpeg->searchstr_len);
            remaining = len;
        }

        /* error or EOF occurred */
        if (ret == AVERROR_EOF) {
            ret = pkt->size > 0 ? pkt->size : AVERROR_EOF;
        } else {
            av_packet_unref(pkt);
        }
    }

    return ret;
}

#define OFFSET(x) offsetof(MPJPEGDemuxContext, x)

#define DEC AV_OPT_FLAG_DECODING_PARAM
const AVOption mpjpeg_options[] = {
    { "strict_mime_boundary",  "require MIME boundaries match", OFFSET(strict_mime_boundary), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, DEC },
    { NULL }
};


static const AVClass mpjpeg_demuxer_class = {
    .class_name     = "MPJPEG demuxer",
    .item_name      = av_default_item_name,
    .option         = mpjpeg_options,
    .version        = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_mpjpeg_demuxer = {
    .name              = "mpjpeg",
    .long_name         = NULL_IF_CONFIG_SMALL("MIME multipart JPEG"),
    .mime_type         = "multipart/x-mixed-replace",
    .extensions        = "mjpg",
    .priv_data_size    = sizeof(MPJPEGDemuxContext),
    .read_probe        = mpjpeg_read_probe,
    .read_header       = mpjpeg_read_header,
    .read_packet       = mpjpeg_read_packet,
    .read_close        = mpjpeg_read_close,
    .priv_class        = &mpjpeg_demuxer_class,
    .flags             = AVFMT_NOTIMESTAMPS,
};


