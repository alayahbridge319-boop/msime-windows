# UI ownership and compatibility

| Surface | Long-term backend | Compatibility scope |
|---|---|---|
| Candidate, floating toolbar, tray menu | msimeui native Direct2D/DirectWrite | `appearance.ui_backend=webview2` explicitly selects existing WebView skins |
| Settings | UiHtml hosted by the separate native settings application | `appearance.settings_ui_backend=d2d` selects the experimental Direct2D frontend; small-window backend settings never change this host |
| Keyboard, handwriting, emoji and voice panels | Their existing native application/presenter | Shared actions/configuration stay in Server; panels do not own input sessions |

`window/ui_backend_policy.h` owns backend selection. D2D is the default; existing
WebView aliases remain readable. Unknown/retired stored values migrate to D2D,
while new invalid configuration writes fail. A backend setting applies at process
startup, preserving the existing controller lifetime and restart behavior.

The candidate worker resolves output text, auxiliary codes, source badges and
translations once into `CandidateViewItem`. Both native and Web renderers consume
these values. HTML escaping and the legacy comma framing are confined to the Web
adapter; native rendering receives plain text. Both click paths enter the existing
`EnqueueCandidateUiAction` queue with the same focus owner checks and action policy.
The ten-entry mouse range is distinct from the 1–9 keyboard shortcuts.

WebView small windows remain a compatibility backend for existing skins. Their
removal requires: a supported conversion path for CSS-dependent external skins;
parity for candidate/context-menu actions, translations and accessibility; and
recorded mixed-DPI, cold-start and signed/uiAccess host validation of the native
replacement. Until those conditions are met, keep compatibility tests and do not
silently reinterpret a configured WebView skin as a native skin. New business
behavior belongs in shared actions/session/view models, not either renderer.

## Direct2D settings window (experimental)

`MetasequoiaImeSettings.exe` has two frontends for the same window. WebView2 hosting
`ui-html/webview2/settings/ime-settings` stays the default; `appearance.settings_ui_backend
= "d2d"` (or `--d2d` on the command line for one run) draws the window with msimeui
instead, in `server/src/settings/native/`. Both frontends speak the same webview contract:
the native window receives `configSnapshot` and the response messages as JSON and posts
`configUpdate` and the request messages through the dispatcher the WebView2 page uses
(`HandleClientMessage` in `settings_app.cpp`), so config paths are still mapped in one
place. Switching the setting reopens the settings process in the other renderer.

The native pages reproduce the HTML/CSS page one to one: each page builder in
`native_page_*.cpp` follows its partial and module, and the controls in
`native_settings_controls.*` name the CSS rule they mirror. A change to a settings page
therefore has to be made on both sides until the WebView2 frontend is retired.

Sciter is not an active product backend. The old unreferenced handwritten D2D
prototype is removed; native small windows use msimeui presenters. The msimeui
library owns generic controls and rendering, never Server configuration, pipe
sessions, dictionary access, or global input state.

## v2 皮肤在两个后端下的渲染

`schema_version = 2` 的皮肤（九宫格贴图面板、自由锚定挂件、面板外空白）两个后端都支持，但走的是不同的实现：

- WebView2 把面板翻成 CSS：`border-image` 铺九宫格，`content_insets_dip` 落成容器内边距，挂件用 `.containerParent` 的伪元素绝对定位。
- D2D 用 `CandidatePresenter` 的本地场景：面板由 `msimeui::Card::SetBackgroundImageNineSlice` 画成九宫格，挂件由 `CandidateSkinOverlay` 这一层浮在卡片上，`outer_insets_dip` 换成窗口在卡片四边多留的空白。

两侧读同一份 `skin.toml`，字段语义必须一致，尤其是 `anchor` 与 `offset_*_dip`：正数向右、向下。改一侧的定位规则要同时改另一侧。

`content_insets_dip` 在两个后端都从面板外缘量起，D2D 侧由卡片内边距承担，不需要再扣边框。
