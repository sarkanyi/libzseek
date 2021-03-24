#include <stdbool.h>    // bool
#include <stddef.h>     // size_t
#include <stdio.h>      // I/O
#include <stdlib.h>     // malloc, free
#include <errno.h>      // errno
#include <string.h>     // memset
#include <pthread.h>    // pthread_mutex*

#include <zstd.h>

#include "archive.h"
#include "seek_table.h"

#define COMPRESSION_LEVEL ZSTD_CLEVEL_DEFAULT
#define COMPRESSION_STRATEGY ZSTD_fast

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

struct nio_archive_writer {
    FILE *fout;
    ZSTD_CCtx *cctx;
    pthread_mutex_t lock;

    size_t frame_uc;    // Current frame bytes (uncompressed)
    size_t frame_cm;    // Current frame bytes (compressed)
    size_t min_frame_size;
    ZSTD_frameLog *fl;
};

nio_archive_writer_t *nio_archive_writer_open(const char *filename,
    int nb_workers, size_t min_frame_size, char errbuf[NIO_ARCHIVE_ERRBUF_SIZE])
{
    nio_archive_writer_t *writer = malloc(sizeof(nio_archive_writer_t));
    if (!writer) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_writer_open: allocate writer");
        goto fail;
    }
    memset(writer, 0, sizeof(*writer));

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_writer_open: create context");
        goto fail_w_writer;
    }
    size_t r = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel,
        COMPRESSION_LEVEL);
    if (ZSTD_isError(r)) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_open: set compression level: %s\n",
            ZSTD_getErrorName(r));
        goto fail_w_cctx;
    }
    // TODO OPT: Don't set strategy?
    r = ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, COMPRESSION_STRATEGY);
    if (ZSTD_isError(r)) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_open: set strategy: %s\n",
            ZSTD_getErrorName(r));
        goto fail_w_cctx;
    }
    r = ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nb_workers);
    if (ZSTD_isError(r)) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_open: set nb of workers: %s\n",
            ZSTD_getErrorName(r));
        goto fail_w_cctx;
    }
    writer->cctx = cctx;
    writer->min_frame_size = min_frame_size;

    int pr = pthread_mutex_init(&writer->lock, NULL);
    if (pr) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_open: initialize mutex: %s\n",
            strerror(pr));
        goto fail_w_cctx;
    }

    ZSTD_frameLog *fl = ZSTD_seekable_createFrameLog(0);
    if (!fl) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_open: create frame log failed\n");
        goto fail_w_lock;
    }
    writer->fl = fl;

    FILE *fout = fopen(filename, "wb");
    if (!fout) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_writer_open: open file");
        goto fail_w_framelog;
    }
    writer->fout = fout;

    return writer;

fail_w_framelog:
    ZSTD_seekable_freeFrameLog(fl);
fail_w_lock:
    pthread_mutex_destroy(&writer->lock);
fail_w_cctx:
    ZSTD_freeCCtx(cctx);
fail_w_writer:
    free(writer);
fail:
    return NULL;
}

/**
 * Flush, close and write current frame. This will block. Should be called with
 * the writer lock held.
 */
static bool end_frame(nio_archive_writer_t *writer)
{
    // TODO: Communicate error info?

    // Allocate output buffer
    size_t buf_len = ZSTD_CStreamOutSize();
    void *buf = malloc(buf_len);
    if (!buf) {
        perror("end_frame: allocate output buffer");
        goto fail;
    }

    ZSTD_inBuffer buffin = {NULL, 0, 0};
    size_t rem = 0;
    do {
        ZSTD_outBuffer buffout = {buf, buf_len, 0};

        // Flush and end frame
        rem = ZSTD_compressStream2(writer->cctx, &buffout, &buffin,
            ZSTD_e_end);
        if (ZSTD_isError(rem)) {
            fprintf(stderr, "end_frame: compress: %s\n",
                ZSTD_getErrorName(rem));
            goto fail_w_buf;
        }

        // Update current frame compressed bytes
        writer->frame_cm += buffout.pos;

        // Write output
        size_t written = fwrite(buffout.dst, 1, buffout.pos, writer->fout);
        if (written < buffout.pos) {
            perror("end_frame: write to file");
            goto fail_w_buf;
        }
    } while (rem > 0);

    // Log frame
    size_t r = ZSTD_seekable_logFrame(writer->fl, writer->frame_cm,
        writer->frame_uc, 0);
    if (ZSTD_isError(r)) {
        fprintf(stderr, "end_frame: log frame: %s\n", ZSTD_getErrorName(r));
        goto fail_w_buf;
    }

    // Reset current frame bytes
    writer->frame_uc = 0;
    writer->frame_cm = 0;

    free(buf);
    return true;

fail_w_buf:
    free(buf);
fail:
    return false;
}

bool nio_archive_writer_close(nio_archive_writer_t *writer,
    char errbuf[NIO_ARCHIVE_ERRBUF_SIZE])
{
    if (writer->frame_uc > 0) {
        // End final frame
        if (!end_frame(writer)) {
            // TODO: Return in errbuf instead.
            fprintf(stderr, "nio_archive_writer_close: end frame failed");
            return false;   // TODO: Continue with the cleanup?
        }
    }

    size_t buf_len = 4096;
    void *buf = malloc(buf_len);
    if (!buf) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_writer_close: allocate output buffer");
        return false;
    }

    // Write seek table
    size_t rem = 0;
    do {
        ZSTD_outBuffer buffout = {buf, buf_len, 0};

        rem = ZSTD_seekable_writeSeekTable(writer->fl, &buffout);
        if (ZSTD_isError(rem)) {
            // TODO: Return in errbuf instead.
            fprintf(stderr, "nio_archive_writer_close: write seek table: %s\n",
                ZSTD_getErrorName(rem));
            return false;
        }

        size_t written = fwrite(buffout.dst, 1, buffout.pos, writer->fout);
        if (written < buffout.pos) {
            // TODO: Return in errbuf instead.
            perror("nio_archive_writer_close: write to file");
            return false;
        }
    } while (rem > 0);
    free(buf);

    if (fclose(writer->fout) == EOF) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_writer_close: close file");
        return false;
    }

    size_t r = ZSTD_seekable_freeFrameLog(writer->fl);
    if (ZSTD_isError(r)) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_close: free frame log: %s\n",
            ZSTD_getErrorName(r));
        return false;
    }

    int pr = pthread_mutex_destroy(&writer->lock);
    if (pr) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_close: destroy mutex: %s\n",
            strerror(pr));
        return false;
    }

    r = ZSTD_freeCCtx(writer->cctx);
    if (ZSTD_isError(r)) {
        // TODO: Return in errbuf instead.
        fprintf(stderr, "nio_archive_writer_close: free context: %s\n",
            ZSTD_getErrorName(r));
        return false;
    }

    free(writer);

    return true;
}

bool nio_archive_write(nio_archive_writer_t *writer, const void *buf,
    size_t len, char errbuf[NIO_ARCHIVE_ERRBUF_SIZE])
{
    int pr = pthread_mutex_lock(&writer->lock);
    if (pr) {
        fprintf(stderr, "nio_archive_write: lock mutex: %s\n",
            strerror(pr));
        goto fail;
    }

    if (writer->frame_uc >= writer->min_frame_size) {
        // End current frame
        // NOTE: This blocks, flushing data dispatched for compression in
        // previous calls.
        if (!end_frame(writer)) {
            // TODO: Return in errbuf instead.
            fprintf(stderr, "nio_archive_write: end frame failed");
            goto fail_w_lock;
        }
    }

    // Allocate output buffer
    size_t bout_len = ZSTD_CStreamOutSize();    // TODO OPT: Tune this according to input len? (see ZSTD_compressBound)
    void *bout = malloc(bout_len);
    if (!bout) {
        // TODO: Return in errbuf instead.
        perror("nio_archive_write: allocate output buffer");
        goto fail_w_lock;
    }

    ZSTD_inBuffer buffin = {buf, len, 0};
    do {
        ZSTD_outBuffer buffout = {bout, bout_len, 0};

        // Dispatch for compression
        size_t rem = ZSTD_compressStream2(writer->cctx, &buffout, &buffin,
            ZSTD_e_continue);
        if (ZSTD_isError(rem)) {
            // TODO: Return in errbuf instead.
            fprintf(stderr, "nio_archive_write: compress: %s\n",
                ZSTD_getErrorName(rem));
            goto fail_w_bout;
        }

        // Update current frame compressed bytes
        writer->frame_cm += buffout.pos;

        // Write output
        size_t written = fwrite(buffout.dst, 1, buffout.pos, writer->fout);
        if (written < buffout.pos) {
            // TODO: Return in errbuf instead.
            perror("nio_archive_write: write to file");
            goto fail_w_bout;
        }
    } while (buffin.pos < buffin.size);

    // Update current frame uncompresed bytes
    writer->frame_uc += len;

    free(bout);

    pr = pthread_mutex_unlock(&writer->lock);
    if (pr) {
        fprintf(stderr, "nio_archive_write: unlock mutex: %s\n",
            strerror(pr));
        goto fail;
    }

    return true;

fail_w_bout:
    free(bout);
fail_w_lock:
    pthread_mutex_unlock(&writer->lock);
fail:
    return false;
}
