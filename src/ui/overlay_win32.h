// SecondSonPC — renders SecondSonPC's menu (a DrawList) over the game window with Win32 layered windows: a dimmed
// backdrop over the client area and the menu surface with per-pixel alpha, both owned by the game window, click-through
// (the game window keeps focus and gets the mouse), never activated. Direct2D draws in software into a DIB and
// DirectWrite lays out the text (Segoe UI, Cyrillic included): no GPU device, no Vulkan, nothing of the GPU backend -
// it works the same with the bridge's renderer and with --gpu-backend sspc. Main thread only. Own code.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/draw_list.h"

namespace SecondSon::Ui {

class Win32Overlay {
public:
    Win32Overlay();
    ~Win32Overlay();
    Win32Overlay(const Win32Overlay&) = delete;
    Win32Overlay& operator=(const Win32Overlay&) = delete;

    /// Direct2D (software) and DirectWrite. false + `error`: the menu cannot be shown.
    bool Init(std::string& error);
    bool Ok() const;

    /// Shows `list` over `owner_hwnd`'s client area (creates/moves/resizes the windows as needed).
    void Present(void* owner_hwnd, const DrawList& list);
    void Hide();
    bool Visible() const;

    /// Text widths with the renderer's fonts.
    TextMeasure& Measure();

    /// While visible, `callback(context)` runs every `period_ms` on this thread (the game window's message loop).
    void SetTimer(void (*callback)(void*), void* context, unsigned period_ms);

    /// Renders `list`'s surface into 32-bit premultiplied BGRA pixels (tests, screenshots).
    bool RenderToPixels(const DrawList& list, std::vector<std::uint32_t>& pixels, unsigned& width, unsigned& height);

    struct Stats {
        std::uint64_t frames = 0;
        double last_ms = 0.0; // the last render + UpdateLayeredWindow
        double max_ms = 0.0;
    };
    Stats GetStats() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace SecondSon::Ui
