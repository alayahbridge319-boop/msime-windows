#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

class WaveOverlay
{
  public:
    enum class CompactStatus
    {
        None,
        Recognizing,
        Processing,
    };

    enum class Action
    {
        Cancel,
        Confirm,
        Copy,
    };

    WaveOverlay();
    ~WaveOverlay();

    WaveOverlay(const WaveOverlay &) = delete;
    WaveOverlay &operator=(const WaveOverlay &) = delete;

    bool init(HINSTANCE instance, std::function<void(Action)> action_handler);
    void shutdown();

    void show();
    void hide();

    void set_listening(bool listening);
    void set_input_level(float level);
    void set_transcript(const std::wstring &text);
    // When false, the overlay stays compact and never paints live ASR text.
    void set_show_transcript(bool show);
    void set_compact_status(CompactStatus status);
    void set_actions_visible(bool visible);
    // 暂存面板：识别到文字但当前没有输入框可提交时显示（正文 + 复制按钮 + 右上角关闭）。
    void set_stash_text(const std::wstring &text);
    void set_stash_visible(bool visible);
    bool stash_visible() const;
    std::wstring stash_text() const;
    void note_stash_copied();

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensure_render_target();
    bool ensure_text_layout(const std::wstring &transcript, float width, float height);
    void release_render_target();
    void release_text_layout();
    void update_wave_levels();
    void draw();
    void update_dpi_scale();
    void update_window_bounds();
    bool hit_test_action(float x, float y, Action &action) const;
    bool hit_test_stash(float x, float y, Action &action) const;

  private:
    static constexpr int kLevelCount = 12;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;

    std::atomic<bool> listening_{false};
    std::atomic<float> input_level_{0.0f};
    std::atomic<bool> show_transcript_{true};
    std::atomic<CompactStatus> compact_status_{CompactStatus::None};
    std::atomic<bool> actions_visible_{false};
    Action pressed_action_ = Action::Confirm;
    bool action_pressed_ = false;
    std::function<void(Action)> action_handler_;
    std::atomic<bool> stash_visible_{false};
    mutable std::mutex stash_mutex_;
    std::wstring stash_text_;
    ULONGLONG stash_copied_until_ms_ = 0;
    std::mutex transcript_mutex_;
    std::wstring transcript_;
    float levels_[kLevelCount] = {};
    float amplitudes_[kLevelCount] = {};
    float phases_[kLevelCount] = {};
    float freqs_[kLevelCount] = {};
    bool light_theme_ = false;

    struct ID2D1Factory *factory_ = nullptr;
    struct ID2D1HwndRenderTarget *render_target_ = nullptr;
    struct ID2D1SolidColorBrush *bar_brush_ = nullptr;
    struct ID2D1SolidColorBrush *bg_brush_ = nullptr;
    struct ID2D1SolidColorBrush *border_brush_ = nullptr;
    struct ID2D1SolidColorBrush *action_bg_brush_ = nullptr;
    struct IDWriteFactory *write_factory_ = nullptr;
    struct IDWriteTextFormat *text_format_ = nullptr;
    struct IDWriteTextFormat *processing_text_format_ = nullptr;
    struct IDWriteTextFormat *stash_text_format_ = nullptr;
    struct IDWriteTextFormat *stash_button_format_ = nullptr;
    struct IDWriteTextLayout *text_layout_ = nullptr;
    std::wstring layout_source_transcript_;
};
