/* Coverage export of a target image, linked only into coverage builds (-fprofile-info-section).
 *
 * At exit the counters of every instrumented object are written to the console as one gcda stream
 * (gcov-tool merge-stream format) in hex:
 *     GCOV-BEGIN
 *     <hex lines>
 *     GCOV-END <length> <crc32>        (both 8 hex digits)
 * decode_gcov.py checks the length and CRC-32 before it merges the stream into the build's .gcda files,
 * so a stream cut short or corrupted on the way never counts. A run that cannot finish the export
 * writes GCOV-ERROR and no GCOV-END. */

#include <gcov.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GCOV_LINE_BYTES 64U

extern const struct gcov_info *const __gcov_info_start[];
extern const struct gcov_info *const __gcov_info_end[];
int _write(int file, const char *buffer, int length);
void _exit(int status);

struct gcov_stream
{
    uint32_t bytes;
    uint32_t crc;
    unsigned pending;
    char line[GCOV_LINE_BYTES * 2U + 1U];
};

static void gcov_put(const char *text, unsigned length)
{
    while (length > 0U)
    {
        int written = _write(1, text, (int)length);
        if (written <= 0)
        {
            return;
        }
        text += written;
        length -= (unsigned)written;
    }
}

static void gcov_put_string(const char *text)
{
    unsigned length = 0U;

    while (text[length] != '\0')
    {
        ++length;
    }
    gcov_put(text, length);
}

static void gcov_flush_line(struct gcov_stream *stream)
{
    if (stream->pending > 0U)
    {
        stream->line[stream->pending * 2U] = '\n';
        gcov_put(stream->line, stream->pending * 2U + 1U);
        stream->pending = 0U;
    }
}

static void gcov_dump_bytes(const void *data, unsigned length, void *arg)
{
    static const char hex[] = "0123456789abcdef";
    struct gcov_stream *stream = arg;
    const uint8_t *byte = data;

    for (unsigned index = 0U; index < length; ++index)
    {
        uint32_t crc = stream->crc ^ byte[index];
        for (unsigned bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
        stream->crc = crc;
        stream->line[stream->pending * 2U] = hex[byte[index] >> 4];
        stream->line[stream->pending * 2U + 1U] = hex[byte[index] & 0x0FU];
        ++stream->bytes;
        if (++stream->pending == GCOV_LINE_BYTES)
        {
            gcov_flush_line(stream);
        }
    }
}

static void gcov_filename(const char *filename, void *arg)
{
    __gcov_filename_to_gcfn(filename, gcov_dump_bytes, arg);
}

/* libgcov asks for memory only for value-profile (top-N) counters, which --coverage does not create. There
 * is no allocation here: libgcov does not check the result, so the export ends without a GCOV-END and the
 * run's coverage is rejected. */
static void *gcov_allocate(unsigned length, void *arg)
{
    (void)length;
    (void)arg;
    gcov_put_string("\nGCOV-ERROR value-profile counters are not supported\n");
    _exit(1);
    return NULL;
}

static void gcov_hex32(char *out, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    for (int digit = 7; digit >= 0; --digit)
    {
        out[digit] = hex[value & 0x0FU];
        value >>= 4;
    }
}

__attribute__((destructor)) static void gcov_export(void)
{
    const struct gcov_info *const *info = __gcov_info_start;
    const struct gcov_info *const *end = __gcov_info_end;
    struct gcov_stream stream = {0U, 0xFFFFFFFFUL, 0U, {0}};
    char trailer[] = "GCOV-END 00000000 00000000\n";

    /* The section bounds come from the linker script; keep the compiler from assuming they are equal. */
    __asm__("" : "+r"(info));
    fflush(NULL);
    gcov_put_string("\nGCOV-BEGIN\n");
    for (; info != end; ++info)
    {
        __gcov_info_to_gcda(*info, gcov_filename, gcov_dump_bytes, gcov_allocate, &stream);
    }
    gcov_flush_line(&stream);
    gcov_hex32(&trailer[9], stream.bytes);
    gcov_hex32(&trailer[18], stream.crc ^ 0xFFFFFFFFUL);
    gcov_put(trailer, sizeof trailer - 1U);
}
