#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <libavformat/avformat.h>
#include <libavutil/aes.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
/* Built-in kid -> CEK table.  Every video has its own key; if no --key /
 * --key-hex is given, the CEI kid selects the entry here.  New contents can
 * be added with the output of `python3 -m pyudrm cek --mp4 file.mp4`. */
typedef struct {
    const char *kid;
    const char *cek;
} UKeyEntry;

static const UKeyEntry UDRM_KEYS[] = {
    { "l6ethtgd000000000000000000000004", "kfyu33b6sj2xoejm" },
    { "o0gfroc3000000000000000000000012", "uj8nrg90n9t2nc9r" },
};

static const char *lookup_cek(const char *kid)
{
    size_t i;
    for (i = 0; i < sizeof(UDRM_KEYS) / sizeof(UDRM_KEYS[0]); i++)
        if (!strcmp(UDRM_KEYS[i].kid, kid))
            return UDRM_KEYS[i].cek;
    return NULL;
}

#define PATTERN_ENC     16      /* encrypted bytes per 160-byte group */
#define PATTERN_CLEAR   144     /* clear bytes per 160-byte group */

static const uint8_t CEI_UUID[16] = {
    0x49, 0x2f, 0x85, 0xaa, 0xc7, 0xa7, 0x4a, 0x7e,
    0xbf, 0x53, 0x01, 0x40, 0xf7, 0x28, 0xc1, 0x06
};

typedef struct {
    int  patched_cei;
    int  decrypted_nals;
    int  key_forced;    /* --key / --key-hex given */
    uint8_t key[16];
    int  have_key;
    struct AVAES *aes;
    char kid[65];       /* kid of the current CEI */
    int  have_kid;
    int  fatal;         /* no CEK for the kid */
} UState;

static int set_key(UState *st, const uint8_t *key)
{
    if (st->aes) {
        av_free(st->aes);
        st->aes = NULL;
    }
    st->aes = av_aes_alloc();
    if (!st->aes)
        return -1;
    if (av_aes_init(st->aes, key, 128, 1) < 0) {
        av_free(st->aes);
        st->aes = NULL;
        return -1;
    }
    memcpy(st->key, key, 16);
    st->have_key = 1;
    return 0;
}

/* A CEI is an H.264 SEI (type 6 / payload type 5) carrying the UDRM UUID,
 * payload starting with 0x82 (or 0x83 for an encrypted CEI).  The TLV tag
 * 'I' holds the licence key id (kid) of this content. */
static int parse_cei_kid(const uint8_t *nal, int len, char kid[65])
{
    const uint8_t *p;
    int plen, flags, v16, off;

    if (len < 19 || nal[0] != 0x06 || nal[1] != 0x05)
        return 0;
    if (memcmp(nal + 3, CEI_UUID, 16) != 0)
        return 0;

    p = nal + 19;
    plen = len - 19;
    if (plen < 7 || (p[0] != 0x82 && p[0] != 0x83))
        return 0;

    flags = p[2];
    v16 = flags & 0x0f;
    off = (flags & 8) ? 4 : 3;
    off += 4 * (v16 & 4) + ((v16 >> 1) & 1);
    if (v16 & 1)
        off += 1;

    while (off + 2 <= plen) {
        int tag = p[off];
        int l = p[off + 1];
        if (off + 2 + l > plen)
            break;
        if (tag == 'I') {
            int n = l < 64 ? l : 64;
            memcpy(kid, p + off + 2, n);
            kid[n] = 0;
            return 1;
        }
        off += 2 + l;
    }
    return 0;
}

static void patch_cei_nal(uint8_t *nal, int len)
{
    if (len < 5)
        return;
    nal[0] = 0x06;
    nal[1] = 0x03;
    nal[2] = (uint8_t)(len - 4);
    memset(nal + 3, 0xff, len - 4);
}

static void decrypt_nal(uint8_t *nal, int len, UState *st)
{
    uint8_t prev[16];
    int pos, remain;

    if (len <= 49 || !st->aes)
        return;

    memcpy(prev, nal + 16, 16);
    pos = 32;
    remain = len - 32;
    while (remain > 0) {
        uint8_t ct[16], pt[16];
        int i, skip;

        if (remain <= PATTERN_ENC)
            break;
        memcpy(ct, nal + pos, 16);
        av_aes_crypt(st->aes, pt, ct, 1, NULL, 1);   /* ECB block decrypt */
        for (i = 0; i < 16; i++)
            nal[pos + i] = pt[i] ^ prev[i];
        memcpy(prev, ct, 16);
        pos += 16;
        remain -= 16;

        skip = remain < PATTERN_CLEAR ? remain : PATTERN_CLEAR;
        pos += skip;
        remain -= skip;
    }
    st->decrypted_nals++;
}

static void decrypt_sample(uint8_t *data, int size, UState *st)
{
    int pos = 0;

    while (pos + 4 <= size) {
        uint32_t len = AV_RB32(data + pos);
        uint8_t *nal;
        int type;

        if (len < 1 || (int64_t)pos + 4 + len > size)
            break;
        nal = data + pos + 4;
        type = nal[0] & 0x1f;

        if (type == 0x06) {
            char kid[65];
            if (parse_cei_kid(nal, (int)len, kid)) {
                if (!st->have_kid || strcmp(st->kid, kid)) {
                    snprintf(st->kid, sizeof(st->kid), "%s", kid);
                    st->have_kid = 1;
                    fprintf(stderr, "CEI kid: %s\n", kid);
                    if (!st->key_forced) {
                        const char *cek = lookup_cek(kid);
                        if (!cek) {
                            fprintf(stderr,
                                    "error: no CEK for kid '%s' (add it to "
                                    "UDRM_KEYS or pass --key)\n", kid);
                            st->fatal = 1;
                        } else if (set_key(st, (const uint8_t *)cek) < 0) {
                            fprintf(stderr, "error: av_aes_init failed\n");
                            st->fatal = 1;
                        } else {
                            fprintf(stderr, "CEK     : %s\n", cek);
                        }
                    }
                }
                patch_cei_nal(nal, (int)len);
                st->patched_cei++;
            }
        } else if (type == 1 || type == 5) {
            decrypt_nal(nal, (int)len, st);
        }
        pos += 4 + len;
    }
}

/* -------------------------------------------------------------------- main */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-q] [--hash] [--key KEY|--key-hex HEX] input.mp4 output.mp4\n"
        "  -q, --quiet     no per-packet logging\n"
        "      --hash      print md5 of every decrypted video packet\n"
        "      --key KEY   16-byte content key as text\n"
        "      --key-hex   16-byte content key as 32 hex digits\n"
        "  (without --key the CEK is selected by the CEI kid from UDRM_KEYS)\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *in_filename = NULL, *out_filename = NULL;
    int quiet = 0, dump_hash = 0;
    int ret, i, stream_mapping_size = 0, stream_index = 0, *stream_mapping = NULL;
    int video_index = -1, n_video = 0;
    const AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket *pkt = NULL;
    UState st;
    uint8_t key[16];
    int key_set = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--key")) {
            if (++i >= argc || strlen(argv[i]) != 16) {
                fprintf(stderr, "--key needs exactly 16 bytes\n");
                return 1;
            }
            memcpy(key, argv[i], 16);
            key_set = 1;
            continue;
        }
        if (!strcmp(argv[i], "--key-hex")) {
            const char *s;
            int j;
            if (++i >= argc || strlen(argv[i]) != 32) {
                fprintf(stderr, "--key-hex needs 32 hex digits\n");
                return 1;
            }
            s = argv[i];
            for (j = 0; j < 16; j++) {
                unsigned v;
                if (sscanf(s + 2 * j, "%2x", &v) != 1) {
                    fprintf(stderr, "bad hex key\n");
                    return 1;
                }
                key[j] = (uint8_t)v;
            }
            key_set = 1;
            continue;
        }
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "--hash")) {
            dump_hash = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!in_filename) {
            in_filename = argv[i];
        } else if (!out_filename) {
            out_filename = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!in_filename || !out_filename) {
        usage(argv[0]);
        return 1;
    }

    memset(&st, 0, sizeof(st));
    st.key_forced = key_set;
    if (key_set && set_key(&st, key) < 0) {
        fprintf(stderr, "av_aes_init failed\n");
        return 1;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        if (st.aes)
            av_free(st.aes);
        return 1;
    }

    ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not open input '%s': %s\n", in_filename, av_err2str(ret));
        goto end;
    }

    /* probing the encrypted stream makes the H.264 decoder log concealment
     * errors; they are harmless, silence them while probing */
    {
        int old_level = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET);
        ret = avformat_find_stream_info(ifmt_ctx, 0);
        av_log_set_level(old_level);
    }
    if (ret < 0) {
        fprintf(stderr, "Failed to retrieve input stream information\n");
        goto end;
    }
    if (!quiet)
        av_dump_format(ifmt_ctx, 0, in_filename, 0);

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ofmt = ofmt_ctx->oformat;

    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping = av_calloc(stream_mapping_size, sizeof(*stream_mapping));
    if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (i = 0; i < (int)ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;
        AVStream *out_stream;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }
        stream_mapping[i] = stream_index++;
        if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
            video_index = i;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;
    }
    if (!quiet)
        av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'\n", out_filename);
            goto end;
        }
    }
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }

    while (1) {
        AVStream *in_stream, *out_stream;

        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0)
            break;

        if (pkt->stream_index >= stream_mapping_size ||
            stream_mapping[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        /* one demuxed video packet == one mp4 sample */
        if (pkt->stream_index == video_index &&
            ifmt_ctx->streams[pkt->stream_index]->codecpar->codec_id == AV_CODEC_ID_H264) {
            if ((ret = av_packet_make_writable(pkt)) < 0) {
                fprintf(stderr, "av_packet_make_writable failed\n");
                break;
            }
            decrypt_sample(pkt->data, pkt->size, &st);
            if (st.fatal) {
                ret = AVERROR_INVALIDDATA;
                break;
            }
            n_video++;

            if (dump_hash) {
                uint8_t md5[16];
                char hex[33];
                av_md5_sum(md5, pkt->data, pkt->size);
                for (i = 0; i < 16; i++)
                    snprintf(hex + i * 2, 3, "%02x", md5[i]);
                printf("%s %d %"PRId64"\n", hex, pkt->size, pkt->pts);
            }
        }

        in_stream  = ifmt_ctx->streams[pkt->stream_index];
        pkt->stream_index = stream_mapping[pkt->stream_index];
        out_stream = ofmt_ctx->streams[pkt->stream_index];

        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    av_write_trailer(ofmt_ctx);

end:
    if (n_video > 0 && !st.key_forced && !st.have_key && !st.fatal) {
        fprintf(stderr, "error: no CEI kid found, cannot select a CEK\n");
        ret = AVERROR_INVALIDDATA;
    }
    if (!quiet && n_video)
        fprintf(stderr, "video samples: %d, CEI patched: %d, NALs decrypted: %d, "
                "kid: %s, key: %s\n", n_video, st.patched_cei, st.decrypted_nals,
                st.have_kid ? st.kid : "(none)",
                st.key_forced ? "custom" :
                (st.have_key ? "kid table" : "none"));

    av_packet_free(&pkt);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    av_freep(&stream_mapping);
    if (st.aes)
        av_free(st.aes);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
