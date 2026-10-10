// A fake monotonic clock for the in-memory platform layers (test/zipos.c,
// test/unzipos.c), for their os_now: it advances by tick milliseconds at
// each reading, so that a program's work between readings takes as long
// as the test says, and counts its readings. Zeroed, it stands still.
typedef struct {
    i64 now;
    i64 tick;
    i32 reads;
} fakeclock;

static i64 fakeclock_read(fakeclock *c)
{
    i64 now = c->now;
    c->now += c->tick;
    c->reads++;
    return now;
}
