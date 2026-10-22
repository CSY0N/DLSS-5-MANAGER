// lf_fg_pace.inl - when DLSS MFG's frames go to the screen (round 28).
//
// Before, the presenter began each pair the moment it arrived and spaced its
// frames from there. Under a game that fills the card pairs arrive unevenly,
// and the trace showed it plainly: a real frame, the next pair's frame in
// between 1 ms after it, then 13 ms of nothing - x2 that moved like the game
// alone, in steps.
//
// Motion is even when every picture is shown the same time after the moment
// of the game it stands for. A real frame stands for the moment it reached
// the desktop (the capture's own time stamp), a frame in between for its
// place between its two real ones. So each is shown at
//     its moment + delay
// and the delay is one figure for all of them, moved only slowly.
//
// The delay has to cover what a pair needs before its first frame in between
// can be shown: from the older real frame's moment until the pair is ready
// (the capture, the motion estimate, NGX), less one step. That need jitters;
// the delay is taken from the recent needs according to the Queue target:
//     0  their median       - least delay, about every second pair comes late
//     1  their 90th percentile and half a millisecond (the default)
//     2  the largest of them and half a step - smoothest, most delay
// A pair that still comes late has its frames shown as soon as they can be;
// the delay follows the needs up at most a millisecond a pair and down a
// twentieth of one - a change of pace far below one percent, not seen.

static int g_fg_queue = -1;   // -1 until LF_FG_QUEUE or a reshape has said

static unsigned FgQueueTarget()
{
    if (g_fg_queue < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_FG_QUEUE", text, sizeof(text));
        g_fg_queue = (len > 0 && len < sizeof(text)) ? std::clamp(atoi(text), 0, 2) : 1;
    }
    return static_cast<unsigned>(g_fg_queue);
}

// The reshape message's bits 21-22: the Queue target plus one, 0 = not said.
static void FgQueueFromResize(uint32_t flags)
{
    if ((flags & kReshapeDisplay) == 0) return;
    const unsigned said = (flags & kReshapeFgQueueMask) >> kReshapeFgQueueShift;
    if (said == 0) return;
    const int want = static_cast<int>(std::min(said - 1u, 2u));
    if (want != static_cast<int>(FgQueueTarget())) Trace("[dlssg] Queue target %d", want);
    g_fg_queue = want;
}

struct FgPace
{
    static constexpr unsigned kNeeds = 96;
    static constexpr unsigned kSettled = 6;   // pairs after which the first guess is put right
    double needs[kNeeds] = {};
    unsigned count = 0, next = 0;
    double delay = 0.0;       // seconds; 0: not learnt yet
    // What drawing a frame into the back buffer takes (the copy and its
    // fence), a real frame's and a frame in between's each: a real one is
    // copied at the screen's size, one in between is scaled up from the size
    // it was made at. Drawn ahead of its time (FgPresenter), so only the
    // first frame of a pair has to count it in.
    double lead[2] = {0.0015, 0.0015};
    double call = 0.0004;     // the Present call itself
    double waited = 0.0;      // for the screen to take the frame before, per Present
    // Since the last summary.
    unsigned pairs = 0, late = 0, skipped = 0, dropped = 0;
    // The usual time between two real frames.
    double usual = 0.0;

    void Forget() { count = next = 0; delay = 0.0; }

    // A pair far longer than the ones around it (round 30): something on a
    // page changed after a rest - a line highlighted, a notice appearing.
    // There is nothing to make smooth between "before" and "after" of that:
    // its frames in between were shown in a burst, the content swept through
    // them in an instant (the "sudden pull"), and its long wait was taken for
    // the delay every later frame then had to keep. Such a pair shows its
    // real frame at once and teaches the pacing nothing. A pace that really
    // became slower stops being odd after a few pairs.
    bool Irregular(double span)
    {
        const bool odd = usual > 0.0 && span > std::max(2.5 * usual, usual + 0.025);
        usual = usual > 0.0 ? usual + (odd ? 0.04 : 0.12) * (span - usual) : span;
        return odd;
    }

    double Wanted(double step) const
    {
        if (count == 0) return delay;
        double sorted[kNeeds];
        std::copy(needs, needs + count, sorted);
        std::sort(sorted, sorted + count);
        const unsigned queue = FgQueueTarget();
        if (queue == 0) return sorted[count / 2];
        if (queue == 1) return sorted[std::min(count - 1, static_cast<unsigned>(0.90 * (count - 1) + 0.5))] + 0.0005;
        return sorted[count - 1] + 0.5 * step;
    }

    // A pair was taken up. need: what its first frame in between asks of the
    // delay; step: the time between two shown frames of it. True when the
    // pair came late for the delay there was.
    bool Took(double need, double step)
    {
        ++pairs;
        needs[next] = need;
        next = (next + 1) % kNeeds;
        count = std::min(count + 1, kNeeds);
        if (delay <= 0.0) { delay = need + 0.25 * step; return false; }
        // The very first pair after a start or a pause is not like the ones
        // that follow it - the generator and the capture are still warming
        // up - and the delay it set was kept: it only comes down a twentieth
        // of a millisecond a pair, which at 20 FPS is a millisecond a second
        // (round 31: 50 ms sliding to 34 over seventeen seconds). Once a few
        // pairs are known the delay is what they ask, set once.
        if (count == kSettled) { delay = Wanted(step); return need > delay; }
        // One late pair does not move the delay by itself (its frames are
        // shown as soon as they can be): raising it at once and coming back
        // down was a second of drift for one hiccup. Lateness that stays
        // shows in the needs, and the delay follows those.
        const double wanted = Wanted(step);
        const bool came_late = need > delay;
        if (came_late) ++late;
        // Within half a millisecond of what is wanted it stays where it is:
        // following every small move of the needs was itself a slow wave of
        // two or three milliseconds in the pace.
        const double off = wanted - delay;
        if (off > 0.0005) delay += std::min(off - 0.0005, came_late ? 0.001 : 0.0003);
        else if (off < -0.0005) delay += std::max(off + 0.0005, -0.00005);
        return came_late;
    }

    void Took(double draw_seconds, double wait_seconds, double call_seconds, bool generated)
    {
        double &l = lead[generated ? 1 : 0];
        l += 0.1 * (std::clamp(draw_seconds, 0.0003, 0.008) - l);
        waited += 0.05 * (std::min(wait_seconds, 0.05) - waited);
        call += 0.05 * (std::clamp(call_seconds, 0.0001, 0.003) - call);
    }
};

// Which of the game's frames are taken, and how many frames in between each
// pair gets, when the game is faster than the screen can show multiplied.
//
// Before, real frames were taken no closer than refresh / multiplier apart,
// by the clock. A game a little faster than that (50 FPS under a 48.5 limit
// at x4 on 200 Hz) was taken later and later until a frame was lost - the
// delay rose and fell twice a second, and each lost frame was a pair twice as
// long: a quarter of the steps on the screen were double ones. A game much
// faster lost its frames without any order.
//
// Now every frame the capture hands over is looked at (its own time stamp),
// the game's frame time is known from them, and a whole choice is made:
// every keep-th frame is taken, each pair gets `count` frames in between,
// whichever shows the most frames the screen can take (4% over its rate is
// let through - the compositor drops those few). The same shown rate from
// more real frames wins: 130 FPS at x2 on 200 Hz is shown as 130 real frames
// rather than 65 real and 65 made. The choice is kept until another is
// clearly better, so a rate at the edge does not flip it.
static struct FgGate
{
    double seen = 0.0, taken = 0.0;      // the last frame looked at, the last taken (QPC seconds)
    double deltas[32] = {};
    unsigned filled = 0, next = 0;
    unsigned keep = 1;                   // every keep-th frame
    int count = -1;                      // frames in between; -1: as asked
    double every = 0.0;                  // the game's frame time
    bool passed_over = false;            // the frame before this take was let go
} g_fg_gate;

static unsigned FgGateCount(unsigned asked)
{
    return g_fg_gate.count < 0 ? asked : std::min(asked, static_cast<unsigned>(g_fg_gate.count));
}

static void FgGateChoose(unsigned asked, double hz, unsigned most_keep = 6)
{
    FgGate &g = g_fg_gate;
    // Their average: the stamps lie on the screen's refresh grid (a 130 FPS
    // game on 200 Hz comes as 5, 10, 5, 10 ms), so no single one says it.
    double sum = 0.0;
    for (unsigned i = 0; i < g.filled; ++i) sum += g.deltas[i];
    g.every = sum / g.filled;
    const double rate = 1.0 / g.every, most = hz * 1.04;
    auto shown = [&](unsigned keep, unsigned count) { return rate / keep * (count + 1); };
    unsigned best_keep = 1, best_count = 0;
    double best = 0.0;
    for (unsigned keep = 1; keep <= most_keep; ++keep)
        for (unsigned count = 0; count <= asked; ++count)
        {
            const double s = shown(keep, count);
            if (s <= most && s > best * 1.001) { best = s; best_keep = keep; best_count = count; }
        }
    if (best <= 0.0) { best_keep = most_keep; best_count = 0; }
    if (g.keep > most_keep) g.keep = most_keep;
    const unsigned now_count = FgGateCount(asked);
    const double current = shown(g.keep, now_count);
    const bool fits = current <= hz * 1.07;
    if (best_keep == g.keep && best_count == now_count) return;
    if (fits && best < current * 1.05) return;
    g.keep = best_keep;
    g.count = static_cast<int>(best_count);
    if (best_keep == 1 && best_count == asked)
        Trace("[dlssg] the game draws %.0f frames a second: every one is taken, x%u", rate, asked + 1);
    else if (best_count == 0)
        Trace("[dlssg] the game draws %.0f frames a second and the screen shows %.0f: no room for frames in between - "
              "its own frames are shown", rate, hz);
    else
        Trace("[dlssg] the game draws %.0f frames a second and the screen shows %.0f: every %s frame is taken, "
              "%u in between (x%u asked) - %.0f shown", rate, hz,
              best_keep == 1 ? "" : best_keep == 2 ? "second" : best_keep == 3 ? "third" : "few",
              best_count, asked + 1, best);
}

// Frames that were the picture before over again (lf_own_motion.inl,
// SourceSeen): their time stamps count frames the game never drew, and
// the gate would choose by a rate that is not the game's - "the game draws
// 114 frames a second: no room for frames in between" for a game drawing 72.
// While they keep coming a repeat cannot be told by its stamp, so every frame
// is taken and looked at, the repeats passed over - and the game's rate is
// counted from the frames that proved new (FgGateFresh), which still says how
// many frames in between the screen has room for. Three seconds after the
// last repeat the gate counts by the stamps again.
static double g_fg_repeat_at = -1e9;
static struct { unsigned asked = 0; double hz = 0.0; bool limited = false; double before = 0.0; } g_fg_repeat;

static bool FgGateRepeats() { return LatencyNow() - g_fg_repeat_at < 3.0; }

static void FgGateRepeat(double now)
{
    if (now - g_fg_repeat_at >= 3.0)
    {
        g_fg_gate.filled = g_fg_gate.next = 0;
        g_fg_gate.keep = 1;
        g_fg_gate.count = -1;
        g_fg_repeat.before = 0.0;
    }
    g_fg_repeat_at = now;
}

// A frame that proved new while repeats come: the game's own pace.
static void FgGateFresh(double stamp)
{
    if (!FgGateRepeats() || g_fg_repeat.hz <= 0.0) return;
    FgGate &g = g_fg_gate;
    const double delta = stamp - g_fg_repeat.before;
    g_fg_repeat.before = stamp;
    if (delta > 0.0015 && delta < 0.1)
    {
        g.deltas[g.next] = delta;
        g.next = (g.next + 1) % 32;
        g.filled = std::min(g.filled + 1, 32u);
    }
    else if (delta >= 0.1 || delta < 0.0) g.filled = g.next = 0;
    if (g_fg_repeat.limited || g_fg_repeat.hz < 24.0) { g.keep = 1; g.count = -1; }
    else if (g.filled >= 12) FgGateChoose(g_fg_repeat.asked, g_fg_repeat.hz, 1);
}

// A frame from the capture with its time stamp: is it taken?
static bool FgGateTakes(double stamp, unsigned asked, double hz, bool limited)
{
    FgGate &g = g_fg_gate;
    if (FgGateRepeats())
    {
        g_fg_repeat.asked = asked; g_fg_repeat.hz = hz; g_fg_repeat.limited = limited;
        g.keep = 1;
        g.seen = g.taken = stamp;
        return true;
    }
    const double delta = stamp - g.seen;
    g.seen = stamp;
    if (delta > 0.0015 && delta < 0.1)
    {
        g.deltas[g.next] = delta;
        g.next = (g.next + 1) % 32;
        g.filled = std::min(g.filled + 1, 32u);
    }
    else if (delta >= 0.1 || delta < 0.0) g.filled = g.next = 0;   // a pause: learnt again
    // An explicit frame limit spreads the frames in between itself (FgPresent).
    if (limited || hz < 24.0) { g.keep = 1; g.count = -1; }
    else if (g.filled >= 12) FgGateChoose(asked, hz);
    if (g.keep > 1 && stamp >= g.taken && stamp - g.taken < (g.keep - 0.5) * g.every) return false;
    g.taken = stamp;
    return true;
}

static void FgGateForget()
{
    g_fg_gate = FgGate();
}

// The moment of the game a real frame stands for. The capture's time stamp is
// when the compositor showed the frame - on the screen's refresh grid: a 130
// FPS game on a 200 Hz desktop arrives as 5, 10, 5, 10 ms, though it drew
// every 7.7. Taken as they are, those stamps put the grid's unevenness into
// every frame in between. So the moments run on a clock of their own: the
// last moment plus the game's average frame time, pulled a little towards the
// stamp each frame - while the stamp is within one refresh of it. Further
// off, it is the game itself that changed pace, and the stamp is taken.
static struct FgClock
{
    double moment = 0.0, before = 0.0;   // this real frame's and the one before it
    double every = 0.0;                  // the average time between taken frames

    double Moment(double stamp, double span, double hz)
    {
        before = moment;
        if (span <= 0.0 || moment <= 0.0) { moment = stamp; every = 0.0; before = 0.0; return moment; }
        // Every second frame instead of every one (the gate): a new pace.
        if (every > 0.0 && (span > every * 1.7 || span < every * 0.6)) every = 0.0;
        every = every > 0.0 ? every + 0.06 * (span - every) : span;
        const double expected = moment + every;
        const double off = stamp - expected;
        const double grid = hz >= 24.0 ? 1.0 / hz : 0.0;
        moment = std::fabs(off) <= grid ? expected + 0.12 * off : stamp;
        if (moment <= before) moment = stamp > before ? stamp : before + 0.001;
        return moment;
    }
} g_fg_clock;
