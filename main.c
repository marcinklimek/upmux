#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>
#include <signal.h>
#include <stdbool.h>

#include <libswscale/swscale.h>
#include <ev.h>
#include <bitstream/mpeg/psi.h>


#include "upump-ev/upump_ev.h"

#include "upipe/ubase.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_block.h"
#include "upipe/ubuf_block_mem.h"
#include "upipe/uclock.h"
#include "upipe/uclock_std.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/umem_pool.h"
#include "upipe/upipe.h"
#include "upipe/uprobe.h"
#include "upipe/uprobe_dejitter.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/uprobe_select_flows.h"
#include "upipe/uprobe_stdio.h"
#include "upipe/uprobe_transfer.h"
#include "upipe/uprobe_ubuf_mem.h"
#include "upipe/uprobe_ubuf_mem_pool.h"
#include "upipe/uprobe_uclock.h"
#include "upipe/uprobe_upump_mgr.h"
#include "upipe/uprobe_uref_mgr.h"
#include "upipe/upump.h"
#include "upipe/uref.h"
#include "upipe/uref_block.h"
#include "upipe/uref_block_flow.h"
#include "upipe/uref_dump.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_pic.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uref_program_flow.h"
#include "upipe/uref_std.h"

#include "upipe-ts/upipe_ts_demux.h"
#include "upipe-ts/upipe_ts_mux.h"
#include "upipe-ts/upipe_ts_pat_decoder.h"
#include "upipe-ts/upipe_ts_pmt_decoder.h"
#include "upipe-ts/upipe_ts_split.h"
#include "upipe-ts/uref_ts_flow.h"

#include "upipe-av/upipe_av.h"
#include "upipe-av/upipe_avcodec_decode.h"
#include "upipe-av/upipe_avcodec_encode.h"
#include "upipe-av/upipe_avformat_sink.h"
#include "upipe-av/upipe_avformat_source.h"
#include "upipe-av/uref_av_flow.h"
#include "upipe-filters/upipe_filter_blend.h"
#include "upipe-filters/upipe_filter_decode.h"
#include "upipe-filters/upipe_filter_format.h"
#include "upipe-filters/uprobe_filter_suggest.h"
#include "upipe-framers/upipe_a52_framer.h"
#include "upipe-framers/upipe_dvbsub_framer.h"
#include "upipe-framers/upipe_h264_framer.h"
#include "upipe-framers/upipe_mpga_framer.h"
#include "upipe-framers/upipe_mpgv_framer.h"
#include "upipe-framers/upipe_telx_framer.h"
#include "upipe-framers/upipe_video_trim.h"
#include "upipe-framers/uref_mpgv.h"

#include "upipe-modules/upipe_aggregate.h"
#include "upipe-modules/upipe_even.h"
#include "upipe-modules/upipe_file_sink.h"
#include "upipe-modules/upipe_file_source.h"
#include "upipe-modules/upipe_noclock.h"
#include "upipe-modules/upipe_null.h"
#include "upipe-modules/upipe_play.h"
#include "upipe-modules/upipe_probe_uref.h"
#include "upipe-modules/upipe_queue_sink.h"
#include "upipe-modules/upipe_queue_source.h"
#include "upipe-modules/upipe_trickplay.h"
#include "upipe-modules/upipe_udp_sink.h"
#include "upipe-modules/upipe_udp_source.h"
#include "upipe-modules/upipe_worker_linear.h"
#include "upipe-modules/upipe_worker_sink.h"
#include "upipe-modules/upipe_worker_source.h"

#include "upipe-pthread/upipe_pthread_transfer.h"
#include "upipe-pthread/uprobe_pthread_upump_mgr.h"
#include "upipe-swresample/upipe_swr.h"
#include "upipe-swscale/upipe_sws.h"

#include "upipe-x264/upipe_x264.h"

#undef MPEG2_X262_ENCODER


#define UMEM_POOL 512
#define UDICT_POOL_DEPTH 500
#define UREF_POOL_DEPTH 500
#define UBUF_POOL_DEPTH 3000
#define UBUF_SHARED_POOL_DEPTH 50
#define UPUMP_POOL 10
#define UPUMP_BLOCKER_POOL 10

#define DEJITTER_DIVIDER        100
#define SRC_OUT_QUEUE_LENGTH    10000
#define XFER_QUEUE              255
#define XFER_POOL               20
#define SRC_OUT_QUEUE_LENGTH    10000
#define DEC_IN_QUEUE_LENGTH     100
#define DEC_OUT_QUEUE_LENGTH    50

#define ENC_IN_QUEUE_LENGTH     100
#define ENC_OUT_QUEUE_LENGTH    50

#define READ_SIZE               (7*188)

/* app options */
int   loglevel   = UPROBE_LOG_WARNING;
char* crf_val    = NULL;
int   only_remux = 0;

static struct uref_mgr *uref_mgr;
static struct upump_mgr *upump_mgr;

static struct upipe_mgr *upipe_noclock_mgr;
static struct upipe_mgr *upipe_vtrim_mgr;
static struct upipe *upipe_even;

static struct uprobe *logger;
static struct uprobe uprobe_src_s;
static struct uprobe *uprobe_dejitter = NULL;
static struct uprobe uprobe_demux_program_s;

static struct uprobe uprobe_mux;

struct upipe_mgr *upipe_avcdec_mgr;
struct upipe_mgr *upipe_avcenc_mgr;
struct upipe_mgr *upipe_ffmt_mgr;
struct upipe_mgr* upipe_filter_blend_mgr;

struct upipe_mgr *upipe_x264_mgr;

struct upipe *upipe_ts_mux;

struct upipe_mgr *upipe_wsrc_mgr = NULL;
struct upipe_mgr *upipe_wlin_mgr = NULL;
struct upipe_mgr *upipe_wsink_mgr = NULL;

struct uclock *uclock = NULL;

char* src_path = NULL;
char* dst_path = NULL;


char* ubase_err_s[] = {
    "UBASE_ERR_NONE",
    "UBASE_ERR_UNKNOWN",
    "UBASE_ERR_ALLOC",
    "UBASE_ERR_UPUMP",
    "UBASE_ERR_UNHANDLED",
    "UBASE_ERR_INVALID",
    "UBASE_ERR_EXTERNAL",
    "UBASE_ERR_BUSY"
};


/* TODO: verify what plumber is. Can be changed? */
bool check_have_output(struct upipe *upipe, uint64_t flow_id)
{
    struct upipe *output = NULL;
    while (ubase_check(upipe_iterate_sub(upipe, &output)) &&
           output != NULL)
    {
        struct uref *flow_def2;
        uint64_t id2;

        if (ubase_check(upipe_get_flow_def(output, &flow_def2)) &&
                ubase_check(uref_flow_get_id(flow_def2, &id2)) &&
                flow_id == id2)
        {
            /* We already have an output. */
            return true;
        }
    }

    return false;
}

/* generic probe */
static int catch_main(struct uprobe *uprobe,
                      struct upipe *upipe,
                      int event,
                      va_list args)
{
    upipe_notice_va(upipe, "CATCH MAIN EVENT %i", event);

    return UBASE_ERR_NONE;
}

static int catch_src(struct uprobe *uprobe,
                     struct upipe *upipe,
                     int event,
                     va_list args)
{
    if (event == UPROBE_SOURCE_END)
    {
        upipe_dbg(upipe, "source end");
        upipe_release(upipe);
    }

    return uprobe_throw_next(uprobe, upipe, event, args);
}

static int catch_mux(struct uprobe *uprobe,
                     struct upipe *upipe,
                     int event,
                     va_list args)
{
    upipe_notice_va(upipe, "CATCH MUX EVENT %i", event);
    return UBASE_ERR_NONE;
}

#ifdef MPEG2_X262_ENCODER
struct upipe* encoder_plumbing(struct upipe *input)
{
    struct urational sar;
    sar.num = 16;
    sar.den = 11;

    struct upipe_mgr *fdec_mgr = upipe_fdec_mgr_alloc();
    struct upipe_mgr *avcdec_mgr = upipe_avcdec_mgr_alloc();
    upipe_fdec_mgr_set_avcdec_mgr(fdec_mgr, avcdec_mgr);
    upipe_mgr_release(avcdec_mgr);
    struct upipe *avcdec = upipe_void_alloc(fdec_mgr,
                                            uprobe_pfx_alloc_va(uprobe_use(logger),
                                                                UPROBE_LOG_VERBOSE, "avcdec video"));
    assert(avcdec != NULL);
    upipe_mgr_release(fdec_mgr);
    upipe_set_option(avcdec, "threads", "4");

    /* decoder thread */
    avcdec = upipe_wlin_alloc(upipe_wlin_mgr,
                              uprobe_pfx_alloc(uprobe_use(logger),
                                               UPROBE_LOG_VERBOSE, "wlin video dec"),
                              avcdec,
                              uprobe_pfx_alloc(uprobe_use(logger),
                                               UPROBE_LOG_VERBOSE, "wlin_x video dec"),
                              DEC_IN_QUEUE_LENGTH, DEC_OUT_QUEUE_LENGTH);
    assert(avcdec != NULL);
    upipe_set_output(input, avcdec);
    upipe_release(avcdec);

    /* encoder */
    struct uref *encoder_flow = uref_pic_flow_alloc_def(uref_mgr, 1);

    assert(encoder_flow != NULL);
    ubase_assert(uref_pic_flow_add_plane(encoder_flow, 1, 1, 1, "y8"));
    ubase_assert(uref_pic_flow_add_plane(encoder_flow, 2, 2, 1, "u8"));
    ubase_assert(uref_pic_flow_add_plane(encoder_flow, 2, 2, 1, "v8"));
    ubase_assert(uref_pic_flow_set_hsize(encoder_flow, 720));
    ubase_assert(uref_pic_flow_set_vsize(encoder_flow, 576));
    ubase_assert(uref_pic_flow_set_sar(encoder_flow, sar));

    struct urational fps;
    fps.num = 25;
    fps.den = 1;

    ubase_assert(uref_pic_flow_set_fps(encoder_flow, fps));

    //TODO: find problem with aspect ratio, currently after encoding 4:3, should follow the input
    struct upipe *encoder = upipe_void_alloc(upipe_x264_mgr,
                                             uprobe_pfx_alloc_va(uprobe_use(logger),
                                                                 loglevel, "x262 enc") );

    assert(encoder != NULL);

    ubase_assert(upipe_set_flow_def(encoder, encoder_flow));
    ubase_assert(upipe_x264_set_default(encoder));
    ubase_assert(upipe_x264_set_default_mpeg2(encoder));
    ubase_assert(upipe_x264_set_default_preset(encoder, "ultrafast", ""));
    ubase_assert(upipe_set_option(encoder, "overscan", "crop"));

    ubase_assert(upipe_set_option(encoder, "crf", crf_val));
    ubase_assert(upipe_set_option(encoder, "tff", "1"));
    ubase_assert(upipe_set_option(encoder, "interlaced", "1"));
    ubase_assert(upipe_set_option(encoder, "sliced-threads", "1"));
    ubase_assert(upipe_set_option(encoder, "bframes", "2"));
    ubase_assert(upipe_set_option(encoder, "keyint", "45"));

    uref_free(encoder_flow);

    /* encoder thread */
    encoder = upipe_wlin_alloc(upipe_wlin_mgr,
                               uprobe_pfx_alloc(uprobe_use(logger),
                                                UPROBE_LOG_VERBOSE, "wlin video enc"),
                               encoder,
                               uprobe_pfx_alloc(uprobe_use(logger),
                                                UPROBE_LOG_VERBOSE, "wlin_x video enc"),
                               ENC_IN_QUEUE_LENGTH, ENC_OUT_QUEUE_LENGTH);
    assert(encoder != NULL);
    upipe_set_output(avcdec, encoder);

    return encoder;
}
#endif

/* TS demux programs */
static int catch_ts_demux_program(struct uprobe *uprobe,
                                  struct upipe *upipe,
                                  int event,
                                  va_list args)
{
    if (event != UPROBE_SPLIT_UPDATE)
        return uprobe_throw_next(uprobe, upipe, event, args);

    struct uref *flow_def = NULL;

    while (ubase_check(upipe_split_iterate(upipe, &flow_def)) &&
           flow_def != NULL)
    {
        uint64_t flow_id;
        ubase_assert(uref_flow_get_id(flow_def, &flow_id));

        if (check_have_output(upipe, flow_id))
            continue;

        const char *def = "(none)";
        uref_flow_get_def(flow_def, &def);

        if ( strstr(def, "dvb_") ||
             strstr(def, "block.ac3.sound.") ||
             strstr(def, "block.eac3.sound."))
        {
            upipe_notice_va(upipe, "skipping flow %"PRIu64" (%s)", flow_id, def);
            continue;
        }

        struct upipe *output = upipe_flow_alloc_sub(upipe,
                                                    uprobe_pfx_alloc_va( uprobe_use(logger),
                                                                         loglevel,
                                                                         "ts demux output %"PRIu64, flow_id),
                                                    flow_def);

#ifdef MPEG2_X262_ENCODER
        if ( !only_remux && strstr(def, "block.h264.pic."))
        {
            output = encoder_plumbing(output);
            assert( output != NULL );
        }
#endif

        output = upipe_void_alloc_output(output, upipe_noclock_mgr,
                                         uprobe_pfx_alloc_va(uprobe_use(logger),
                                                             loglevel,
                                                             "noclock %"PRIu64, flow_id));
        assert(output != NULL);


        struct upipe *upipe_ts_mux_program;
        ubase_assert(upipe_get_output(upipe, &upipe_ts_mux_program));

        output = upipe_void_chain_output_sub(output,
                                             upipe_ts_mux_program,
                                             uprobe_pfx_alloc_va(uprobe_use(logger),
                                                                 loglevel,
                                                                 "mux input %"PRIu64, flow_id));

        assert(output != NULL);
        upipe_release(output);
    }

    return UBASE_ERR_NONE;
}

static int catch_ts_demux(struct uprobe *uprobe,
                          struct upipe *upipe,
                          int event,
                          va_list args)
{
    if (event != UPROBE_SPLIT_UPDATE)
        return uprobe_throw_next(uprobe, upipe, event, args);

    struct uref *flow_def = NULL;

    while (ubase_check(upipe_split_iterate(upipe, &flow_def)) && flow_def != NULL)
    {
        uint64_t flow_id;
        ubase_assert(uref_flow_get_id(flow_def, &flow_id));

        if (check_have_output(upipe, flow_id))
            continue;

        struct upipe* program = upipe_flow_alloc_sub(upipe,
                                                     uprobe_pfx_alloc_va(&uprobe_demux_program_s,
                                                                         loglevel,
                                                                         "ts demux program %"PRIu64,
                                                                         flow_id), flow_def);
        assert(program != NULL);

        struct upipe *upipe_ts_mux_local;
        ubase_assert(upipe_get_output(upipe, &upipe_ts_mux_local));
        assert(upipe_ts_mux_local != NULL);


        program = upipe_void_alloc_output_sub(program,
                                              upipe_ts_mux_local,
                                              uprobe_pfx_alloc_va(uprobe_use(logger),
                                                                  loglevel,
                                                                  "ts mux program %"PRIu64, flow_id));

        upipe_attach_uclock(program);
        assert(program != NULL);
        upipe_release(program);
    }

    return UBASE_ERR_NONE;
}


/* threads management */
static struct upump_mgr *upump_mgr_alloc(void)
{
    struct ev_loop *loop = ev_loop_new(0);
    struct upump_mgr *upump_mgr = upump_ev_mgr_alloc(loop, UPUMP_POOL,
                                                     UPUMP_BLOCKER_POOL);
    assert(upump_mgr != NULL);
    upump_mgr_set_opaque(upump_mgr, loop);
    return upump_mgr;
}

static void upump_mgr_work(struct upump_mgr *upump_mgr)
{
    struct ev_loop *loop = upump_mgr_get_opaque(upump_mgr, struct ev_loop *);
    ev_loop(loop, 0);
}

static void upump_mgr_free(struct upump_mgr *upump_mgr)
{
    struct ev_loop *loop = upump_mgr_get_opaque(upump_mgr, struct ev_loop *);
    ev_loop_destroy(loop);
}

/* ------------------------- */

void usage(char* app)
{
    fprintf(stderr, "%s [-d debug] [-v verbose] [-r only remux] <in stream> <out stream> <crf value 0-69, lower = better quality>\n", app);
    exit(EXIT_FAILURE);
}

void parse_options(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "vdr")) != -1)
    {
        switch (opt)
        {
        case 'd':
            loglevel = UPROBE_LOG_DEBUG;
            break;
        case 'v':
            loglevel = UPROBE_LOG_VERBOSE;
            break;
        case 'r':
            only_remux = 1;
            break;
        default:
            usage(argv[0]);
        }
    }

    if ( (optind >= argc - 1) || (optind+3) > argc )
    {
        usage(argv[0]);
    }

    //TODO: validate parameters!
    src_path = argv[optind++];
    dst_path = argv[optind++];
    crf_val = argv[optind++];
}


int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* parse options */
    parse_options(argc, argv);

    struct ev_loop *loop = ev_default_loop(0);

    struct umem_mgr *umem_mgr = umem_pool_mgr_alloc_simple(UMEM_POOL);
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH, umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(uref_mgr != NULL);
    upump_mgr = upump_ev_mgr_alloc(loop, UPUMP_POOL, UPUMP_BLOCKER_POOL);
    assert(upump_mgr != NULL);

    uclock = uclock_std_alloc(0);

    struct uprobe uprobe_s;
    uprobe_init(&uprobe_s, catch_main, NULL);
    logger = uprobe_stdio_alloc(&uprobe_s, stdout, loglevel);
    assert(logger != NULL);
    logger = uprobe_uclock_alloc(logger, uclock);
    assert(logger != NULL);

    logger = uprobe_uref_mgr_alloc(logger, uref_mgr);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH, UBUF_POOL_DEPTH);
    assert(logger != NULL);
    logger = uprobe_pthread_upump_mgr_alloc(logger);
    assert(logger != NULL);
    uprobe_pthread_upump_mgr_set(logger, upump_mgr);

    logger = uprobe_dejitter_alloc(uprobe_use(logger),
                                   DEJITTER_DIVIDER);
    assert(logger != NULL);

    /* upipe-av */
    if (unlikely(!upipe_av_init(false,
                                uprobe_pfx_alloc(uprobe_use(logger),
                                                 loglevel, "av"))))
    {
        uprobe_err_va(logger, NULL, "unable to init av");
        exit(EXIT_FAILURE);
    }


    /* worker threads */
    struct upipe_mgr *src_xfer_mgr = upipe_pthread_xfer_mgr_alloc(XFER_QUEUE,
                                                                  XFER_POOL, uprobe_use(logger), upump_mgr_alloc,
                                                                  upump_mgr_work, upump_mgr_free, NULL, NULL);
    assert(src_xfer_mgr != NULL);
    upipe_wsrc_mgr = upipe_wsrc_mgr_alloc(src_xfer_mgr);
    assert(upipe_wsrc_mgr != NULL);
    upipe_mgr_release(src_xfer_mgr);

    struct upipe_mgr *dec_xfer_mgr = upipe_pthread_xfer_mgr_alloc(XFER_QUEUE,
                                                                  XFER_POOL, uprobe_use(logger), upump_mgr_alloc,
                                                                  upump_mgr_work, upump_mgr_free, NULL, NULL);
    assert(dec_xfer_mgr != NULL);
    upipe_wlin_mgr = upipe_wlin_mgr_alloc(dec_xfer_mgr);
    assert(upipe_wlin_mgr != NULL);
    upipe_mgr_release(dec_xfer_mgr);



    upump_mgr_release(upump_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);

    upipe_noclock_mgr = upipe_noclock_mgr_alloc();
    assert(upipe_noclock_mgr != NULL);
    upipe_vtrim_mgr = upipe_vtrim_mgr_alloc();
    assert(upipe_vtrim_mgr != NULL);

    struct upipe_mgr *upipe_even_mgr = upipe_even_mgr_alloc();
    assert(upipe_even_mgr != NULL);
    upipe_even = upipe_void_alloc(upipe_even_mgr,
                                  uprobe_pfx_alloc(uprobe_use(logger),
                                                   loglevel, "even"));
    assert(upipe_even != NULL);
    upipe_mgr_release(upipe_even_mgr);




    /* udp source */
    unsigned int src_out_queue_length = SRC_OUT_QUEUE_LENGTH;

    struct uprobe *uprobe_src = uprobe_xfer_alloc(uprobe_use(logger));
    uprobe_xfer_add(uprobe_src, UPROBE_XFER_VOID, UPROBE_SOURCE_END, 0);

    struct upipe_mgr *upipe_udpsrc_mgr = upipe_udpsrc_mgr_alloc();
    assert(upipe_udpsrc_mgr != NULL);
    struct upipe* upipe_src = upipe_void_alloc(upipe_udpsrc_mgr,
                                               uprobe_pfx_alloc(uprobe_src,
                                                                loglevel, "udpsrc"));
    upipe_mgr_release(upipe_udpsrc_mgr);

    if (upipe_src == NULL || !ubase_check(upipe_set_uri(upipe_src, src_path)))
        return EXIT_FAILURE;
    upipe_attach_uclock(upipe_src);


    uprobe_init(&uprobe_src_s, catch_src, uprobe_use(logger));

    /* move to thread */
    upipe_src = upipe_wsrc_alloc(upipe_wsrc_mgr,
                                 uprobe_pfx_alloc(uprobe_use(&uprobe_src_s),
                                                  loglevel, "wsrc"),
                                 upipe_src,
                                 uprobe_pfx_alloc(uprobe_use(logger),
                                                  loglevel, "wsrc_x"),
                                 src_out_queue_length);




    /* TS demux */
    uprobe_init(&uprobe_demux_program_s, catch_ts_demux_program, uprobe_use(logger));
    struct uprobe uprobe_ts_demux_s;
    uprobe_init(&uprobe_ts_demux_s, catch_ts_demux, uprobe_use(logger));

    struct upipe_mgr *upipe_mpgvf_mgr = upipe_mpgvf_mgr_alloc();
    assert(upipe_mpgvf_mgr != NULL);
    struct upipe_mgr *upipe_h264f_mgr = upipe_h264f_mgr_alloc();
    assert(upipe_h264f_mgr != NULL);
    struct upipe_mgr *upipe_mpgaf_mgr = upipe_mpgaf_mgr_alloc();
    assert(upipe_mpgaf_mgr != NULL);
    struct upipe_mgr *upipe_a52f_mgr = upipe_a52f_mgr_alloc();
    assert(upipe_a52f_mgr != NULL);
    struct upipe_mgr *upipe_telxf_mgr = upipe_telxf_mgr_alloc();
    assert(upipe_telxf_mgr != NULL);
    struct upipe_mgr *upipe_dvbsubf_mgr = upipe_dvbsubf_mgr_alloc();
    assert(upipe_dvbsubf_mgr != NULL);


    struct upipe_mgr *upipe_ts_demux_mgr = upipe_ts_demux_mgr_alloc();
    assert(upipe_ts_demux_mgr != NULL);

    ubase_assert(upipe_ts_demux_mgr_set_mpgvf_mgr(upipe_ts_demux_mgr, upipe_mpgvf_mgr));
    ubase_assert(upipe_ts_demux_mgr_set_mpgaf_mgr(upipe_ts_demux_mgr, upipe_mpgaf_mgr));
    ubase_assert(upipe_ts_demux_mgr_set_a52f_mgr(upipe_ts_demux_mgr,  upipe_a52f_mgr));
    ubase_assert(upipe_ts_demux_mgr_set_telxf_mgr(upipe_ts_demux_mgr, upipe_telxf_mgr));
    ubase_assert(upipe_ts_demux_mgr_set_dvbsubf_mgr(upipe_ts_demux_mgr,upipe_dvbsubf_mgr));
    ubase_assert(upipe_ts_demux_mgr_set_h264f_mgr(upipe_ts_demux_mgr, upipe_h264f_mgr));

    struct upipe *upipe_ts = upipe_void_alloc_output(upipe_src,
                                                     upipe_ts_demux_mgr,
                                                     uprobe_pfx_alloc(&uprobe_ts_demux_s,
                                                                      UPROBE_LOG_VERBOSE,
                                                                      "ts demux"));

    assert(upipe_ts != NULL);

    upipe_mgr_release(upipe_ts_demux_mgr);
    upipe_mgr_release(upipe_mpgvf_mgr);
    upipe_mgr_release(upipe_h264f_mgr);
    upipe_mgr_release(upipe_mpgaf_mgr);
    upipe_mgr_release(upipe_a52f_mgr);
    upipe_mgr_release(upipe_udpsrc_mgr);
    upipe_mgr_release(upipe_telxf_mgr);
    upipe_mgr_release(upipe_dvbsubf_mgr);


    /* AV */
    upipe_x264_mgr = upipe_x264_mgr_alloc();

    struct upipe_mgr *upipe_swr_mgr = upipe_swr_mgr_alloc();
    struct upipe_mgr *upipe_sws_mgr = upipe_sws_mgr_alloc();
    upipe_filter_blend_mgr = upipe_filter_blend_mgr_alloc();

    upipe_avcdec_mgr = upipe_avcdec_mgr_alloc();
    upipe_avcenc_mgr = upipe_avcenc_mgr_alloc();
    upipe_ffmt_mgr = upipe_ffmt_mgr_alloc();

    upipe_ffmt_mgr_set_sws_mgr(upipe_ffmt_mgr, upipe_sws_mgr);
    upipe_ffmt_mgr_set_swr_mgr(upipe_ffmt_mgr, upipe_swr_mgr);
    upipe_ffmt_mgr_set_deint_mgr(upipe_ffmt_mgr, upipe_filter_blend_mgr);

    uprobe_init(&uprobe_mux, catch_mux, uprobe_use(logger));


    /* TS mux */
    struct upipe_mgr *upipe_ts_mux_mgr = upipe_ts_mux_mgr_alloc();
    assert(upipe_ts_mux_mgr != NULL);

    upipe_ts_mux = upipe_void_chain_output(upipe_ts,
                                           upipe_ts_mux_mgr,
                                           uprobe_pfx_alloc(uprobe_use(logger),
                                                            loglevel,
                                                            "ts mux"));
    assert(upipe_ts_mux != NULL);

    upipe_mgr_release(upipe_ts_mux_mgr);
    ubase_assert(upipe_ts_mux_set_mode(upipe_ts_mux, UPIPE_TS_MUX_MODE_CBR));
    ubase_assert(upipe_ts_mux_set_cr_prog(upipe_ts_mux, 0));
    upipe_ts_mux_set_pcr_interval(upipe_ts_mux, UCLOCK_FREQ/30);

    /* TODO: this should be the app argument */
    upipe_ts_mux_set_octetrate(upipe_ts_mux, 19000000/8);

    struct upipe_mgr *upipe_aggregate_mgr = upipe_agg_mgr_alloc();
    assert(upipe_aggregate_mgr != NULL);

    struct upipe *upipe_aggregate = upipe_void_chain_output(upipe_ts_mux,
                                                            upipe_aggregate_mgr,
                                                            uprobe_pfx_alloc(uprobe_use(logger),
                                                                             loglevel,
                                                                             "aggregate sink"));
    assert(upipe_aggregate != NULL);


    /* udp sink */
    struct upipe_mgr *upipe_udpsink_mgr = upipe_udpsink_mgr_alloc();
    assert(upipe_udpsink_mgr != NULL);
    upipe_ts = upipe_void_chain_output(upipe_aggregate,
                                       upipe_udpsink_mgr,
                                       uprobe_pfx_alloc(uprobe_use(logger),
                                                        loglevel, "udp sink"));
    assert(upipe_ts != NULL);

    if (!ubase_check(upipe_udpsink_set_uri(upipe_ts, dst_path, 0)))
    {
        return EXIT_FAILURE;
    }


    upipe_mgr_release(upipe_udpsink_mgr);
    upipe_release(upipe_ts);

    ev_loop(loop, 0);

    upipe_release(upipe_even);
    uprobe_release(logger);
    uprobe_clean(&uprobe_demux_program_s);
    uprobe_clean(&uprobe_ts_demux_s);
    uprobe_clean(&uprobe_src_s);
    uprobe_clean(&uprobe_s);

    ev_default_destroy();
    return 0;
}
