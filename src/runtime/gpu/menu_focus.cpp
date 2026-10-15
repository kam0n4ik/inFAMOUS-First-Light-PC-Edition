// SecondSonPC — the game's chosen menu item from the presented frame (see menu_focus.h).
#include "runtime/gpu/menu_focus.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace SecondSon::Gpu::MenuFocus {

namespace {

std::atomic<bool> g_wanted{false};
std::mutex g_mutex;
Row g_latest;
u64 g_seq = 0;

bool Yellow(const u8* p) {
    // the game's yellow (~245,184,0) under its text anti-aliasing; white and grey items have b close to r
    return p[0] >= 170 && p[1] >= 110 && p[1] <= 225 && p[2] <= 110 && p[0] - p[2] >= 120;
}

} // namespace

void SetWanted(bool wanted) {
    g_wanted.store(wanted, std::memory_order_relaxed);
    if (!wanted) {
        std::scoped_lock lock{g_mutex};
        g_latest.valid = false;
    }
}

bool Wanted() {
    return g_wanted.load(std::memory_order_relaxed);
}

Row FindFocus(const u8* rgba, u32 w, u32 h, float y0, float scale) {
    Row best;
    best.valid = true;
    u32 run_start = 0, run_len = 0, gap = 0, run_weight = 0, best_weight = 0, last_yellow_row = 0;
    const auto close_run = [&] {
        if (run_len > 0 && run_weight > best_weight) {
            best_weight = run_weight;
            best.found = true;
            best.top = y0 + static_cast<float>(run_start) * scale;
            best.bottom = y0 + static_cast<float>(last_yellow_row + 1) * scale;
        }
        run_len = 0;
        run_weight = 0;
    };
    for (u32 y = 0; y < h; ++y) {
        u32 n = 0;
        for (u32 x = 0; x < w; ++x) {
            n += Yellow(rgba + (u64{y} * w + x) * 4) ? 1 : 0;
        }
        if (n >= 3) {
            if (run_len == 0) {
                run_start = y;
            }
            run_len = y - run_start + 1;
            run_weight += n;
            last_yellow_row = y;
            gap = 0;
        } else if (run_len > 0 && ++gap > 4) { // a line of text has no gaps of more than a few rows
            close_run();
        }
    }
    close_run();
    return best;
}

void FindThumb(const u8* bar, u32 bw, u32 h, float y0, float scale, Row& row) {
    // the longest run of rows whose texels are all the thumb's grey
    u32 best_start = 0, best_len = 0, start = 0, len = 0;
    for (u32 y = 0; y < h; ++y) {
        bool grey = bw > 0;
        for (u32 x = 0; x < bw && grey; ++x) {
            const u8* p = bar + (u64{y} * bw + x) * 4;
            const int lo = std::min({p[0], p[1], p[2]}), hi = std::max({p[0], p[1], p[2]});
            grey = lo >= 62 && hi <= 100 && hi - lo <= 12;
        }
        if (grey) {
            start = len == 0 ? y : start;
            ++len;
            if (len > best_len) {
                best_len = len;
                best_start = start;
            }
        } else {
            len = 0;
        }
    }
    row.bar = best_len * scale >= 12;
    if (row.bar) {
        row.thumb_top = y0 + static_cast<float>(best_start) * scale;
        row.thumb_bottom = y0 + static_cast<float>(best_start + best_len) * scale;
    }
}

void Publish(const u8* rgba, u32 w, const u8* bar, u32 bw, u32 h, float y0, float scale) {
    Row r = FindFocus(rgba, w, h, y0, scale);
    FindThumb(bar, bw, h, y0, scale, r);
    std::scoped_lock lock{g_mutex};
    if (!g_wanted.load(std::memory_order_relaxed)) {
        return;
    }
    r.seq = ++g_seq;
    g_latest = r;
}

Row Latest() {
    std::scoped_lock lock{g_mutex};
    return g_latest;
}

} // namespace SecondSon::Gpu::MenuFocus
