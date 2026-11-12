// Observable sink tests: exact rows, overflow, repeated and concurrent drains.
#include "../nvn/nvn_tsv_stream.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static off_t length(FILE *f) { struct stat st; assert(!fstat(fileno(f), &st)); return st.st_size; }
static void close_stream(nvn_tsv_stream *s) {
    assert(!fclose(s->file)); free(s->bytes);
    assert(!pthread_mutex_destroy(&s->append_mu)); assert(!pthread_mutex_destroy(&s->drain_mu));
}
static void exact(int deferred) {
    nvn_tsv_stream s = NVN_TSV_STREAM_INIT;
    FILE *file = tmpfile(); assert(file);
    assert(!nvn_tsv_stream_init(&s, file, deferred ? 4096 : 0));
    nvn_tsv_stream_printf(&s, "frame\tname\tvalue\n");
    nvn_tsv_stream_printf(&s, "%llu\t%s\t%.3f\n", 123ull, "@frame_ms", 16.6667);
    nvn_tsv_stream_printf(&s, "%llu\t%s\t%llu\n", 123ull, "nvnDraw", 18446744073709551615ull);
    if (deferred) assert(length(file) == 0);
    assert(!nvn_tsv_stream_drain(&s)); off_t first = length(file);
    assert(!nvn_tsv_stream_drain(&s)); assert(length(file) == first);
    nvn_tsv_stream_printf(&s, "124\t@frame_ms\t8.333\n");
    assert(!nvn_tsv_stream_drain(&s));
    assert(!fseek(file, 0, SEEK_SET)); char result[256] = {0}; assert(fread(result, 1, sizeof result - 1, file) > 0);
    assert(!strcmp(result, "frame\tname\tvalue\n123\t@frame_ms\t16.667\n123\tnvnDraw\t18446744073709551615\n124\t@frame_ms\t8.333\n"));
    close_stream(&s);
}
static void overflow(void) {
    nvn_tsv_stream s = NVN_TSV_STREAM_INIT;
    FILE *file = tmpfile(); assert(file); assert(!nvn_tsv_stream_init(&s, file, 4));
    nvn_tsv_stream_printf(&s, "abc\n"); nvn_tsv_stream_printf(&s, "X\n");
    char huge[600]; memset(huge, 'x', sizeof huge - 1); huge[sizeof huge - 1] = 0;
    nvn_tsv_stream_printf(&s, "%s\n", huge);
    assert(s.used == 4 && s.dropped == 2 && length(file) == 0);
    assert(!nvn_tsv_stream_drain(&s)); off_t first = length(file);
    assert(!nvn_tsv_stream_drain(&s)); assert(length(file) == first);
    assert(!fseek(file, 0, SEEK_SET)); char result[128] = {0}; assert(fread(result, 1, sizeof result - 1, file) > 0);
    assert(!strcmp(result, "abc\n# deferred TSV rows dropped: 2 (capacity 4 bytes)\n"));
    close_stream(&s);
}
enum { WRITERS = 4, ROWS = 4000 };
static nvn_tsv_stream concurrent = NVN_TSV_STREAM_INIT;
static _Atomic int finished;
static void *produce(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < ROWS; i++) nvn_tsv_stream_printf(&concurrent, "%u\t%u\n", id, i);
    atomic_fetch_add(&finished, 1); return NULL;
}
static void *drain(void *arg) {
    (void)arg;
    while (atomic_load(&finished) != WRITERS) { assert(!nvn_tsv_stream_drain(&concurrent)); usleep(100); }
    assert(!nvn_tsv_stream_drain(&concurrent)); return NULL;
}
static void threaded(void) {
    FILE *file = tmpfile(); assert(file); assert(!nvn_tsv_stream_init(&concurrent, file, 1024 * 1024));
    pthread_t writer[WRITERS], reader[2];
    for (int i = 0; i < 2; i++) assert(!pthread_create(&reader[i], NULL, drain, NULL));
    for (uintptr_t i = 0; i < WRITERS; i++) assert(!pthread_create(&writer[i], NULL, produce, (void *)i));
    for (int i = 0; i < WRITERS; i++) assert(!pthread_join(writer[i], NULL));
    for (int i = 0; i < 2; i++) assert(!pthread_join(reader[i], NULL));
    assert(!concurrent.dropped); assert(!fseek(file, 0, SEEK_SET)); unsigned char seen[WRITERS][ROWS] = {{0}};
    char line[80]; unsigned count = 0;
    while (fgets(line, sizeof line, file)) {
        unsigned id, row; char extra;
        assert(sscanf(line, "%u\t%u%c", &id, &row, &extra) == 3 && extra == '\n');
        assert(id < WRITERS && row < ROWS && !seen[id][row]); seen[id][row] = 1; count++;
    }
    assert(count == WRITERS * ROWS); close_stream(&concurrent);
}
int main(void) { exact(0); exact(1); overflow(); threaded(); puts("TSV stream: exact output, bounded overflow, repeated/concurrent drains passed"); return 0; }
