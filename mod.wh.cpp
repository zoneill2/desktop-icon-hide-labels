// ==WindhawkMod==
// @id              desktop-icon-hide-labels
// @name            Hide Desktop Icon Labels
// @description     Hides the text labels beneath desktop icons and centers the icon within the selection bounds.
// @version         1.1.0
// @author          YourName
// @github          https://github.com/YourName
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hide Desktop Icon Labels

Hides the text name labels displayed beneath desktop icons, leaving only the
icons themselves visible. The icon is centered within the selection/highlight
boundary so clicking and rubber-band selecting behave correctly.

Tooltips still work on hover, and the actual file/shortcut names are **not**
changed in any way.

## Notes

- Works on Windows 10 and Windows 11.
- Icon tooltips still work normally.
- Renaming / searching for files on the desktop is unaffected.
- The mod targets `explorer.exe` only.
- Right-click the desktop and choose Refresh if icons don't update immediately.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <commctrl.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HWND    g_desktopLV        = nullptr;
static WNDPROC g_origLVProc       = nullptr;

static HWND    g_defView          = nullptr;
static WNDPROC g_origDefViewProc  = nullptr;

// Thread-local paint-suppression flag (set while the desktop LV is painting)
thread_local bool g_suppressDesktopText = false;

// ---------------------------------------------------------------------------
// Subclass proc for the desktop SysListView32
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DesktopLVSubclassProc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    // Wrap paint messages with the text-suppression flag so DrawTextW_Hook
    // knows to silently drop label draw calls.
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        g_suppressDesktopText = true;
        LRESULT r = CallWindowProcW(g_origLVProc, hwnd, msg, wParam, lParam);
        g_suppressDesktopText = false;
        return r;
    }

    // Shrink the BOUNDS / SELECTBOUNDS rect to match only the icon image.
    // This makes the selection highlight box and hit-testing wrap the icon
    // instead of including the (now invisible) label space below it.
    case LVM_GETITEMRECT: {
        // The LVIR_* code is passed in prc->left before the call.
        RECT* prc  = reinterpret_cast<RECT*>(lParam);
        int   code = prc ? prc->left : -1;
        LRESULT r  = CallWindowProcW(g_origLVProc, hwnd, msg, wParam, lParam);

        if (r && prc && (code == LVIR_BOUNDS || code == LVIR_SELECTBOUNDS)) {
            // Get the icon-only rect so we know the real icon dimensions.
            RECT rcIcon = { LVIR_ICON };
            if (CallWindowProcW(g_origLVProc, hwnd, LVM_GETITEMRECT,
                                wParam, reinterpret_cast<LPARAM>(&rcIcon))) {
                int iconW = rcIcon.right  - rcIcon.left;
                int iconH = rcIcon.bottom - rcIcon.top;
                int cellCX = (prc->left + prc->right) / 2;

                prc->left   = cellCX - iconW / 2;
                prc->right  = cellCX + iconW / 2;
                prc->top    = rcIcon.top;
                prc->bottom = rcIcon.top + iconH;
            }
        }
        return r;
    }

    case WM_DESTROY:
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origLVProc));
        g_desktopLV  = nullptr;
        g_origLVProc = nullptr;
        break;
    }

    return CallWindowProcW(g_origLVProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Subclass proc for SHELLDLL_DefView (parent of the LV).
// Intercepts WM_NOTIFY / NM_CUSTOMDRAW to re-center the icon image
// vertically within the full item cell while painting.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DefViewSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NOTIFY && g_desktopLV) {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);

        if (hdr && hdr->hwndFrom == g_desktopLV && hdr->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);

            switch (cd->nmcd.dwDrawStage) {

            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT: {
                int   item     = static_cast<int>(cd->nmcd.dwItemSpec);
                RECT  rcBounds = { LVIR_BOUNDS };
                RECT  rcIcon   = { LVIR_ICON   };

                // Query through the original proc to get un-patched rects.
                CallWindowProcW(g_origLVProc, g_desktopLV, LVM_GETITEMRECT,
                                item, reinterpret_cast<LPARAM>(&rcBounds));
                CallWindowProcW(g_origLVProc, g_desktopLV, LVM_GETITEMRECT,
                                item, reinterpret_cast<LPARAM>(&rcIcon));

                if (!IsRectEmpty(&rcBounds) && !IsRectEmpty(&rcIcon)) {
                    int boundsH = rcBounds.bottom - rcBounds.top;
                    int boundsW = rcBounds.right  - rcBounds.left;
                    int iconH   = rcIcon.bottom   - rcIcon.top;
                    int iconW   = rcIcon.right     - rcIcon.left;

                    // Center icon in the full cell
                    int newTop  = rcBounds.top  + (boundsH - iconH) / 2;
                    int newLeft = rcBounds.left + (boundsW - iconW) / 2;

                    cd->nmcd.rc.top    = newTop;
                    cd->nmcd.rc.bottom = newTop  + iconH;
                    cd->nmcd.rc.left   = newLeft;
                    cd->nmcd.rc.right  = newLeft + iconW;
                }

                // CDRF_NEWFONT causes comctl32 to re-read the rc we just wrote.
                return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
            }

            default:
                break;
            }
        }
    }

    return CallWindowProcW(g_origDefViewProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Find and subclass the desktop LV and its DefView parent
// ---------------------------------------------------------------------------
static void TrySubclassDesktopWindows() {
    if (g_desktopLV) return;

    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return;

    auto findPair = [](HWND parent, HWND* outDefView) -> HWND {
        HWND dv = FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!dv) return nullptr;
        HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
        if (lv && outDefView) *outDefView = dv;
        return lv;
    };

    HWND defView = nullptr;
    HWND lv = findPair(progman, &defView);

    if (!lv) {
        HWND workerW = nullptr;
        while ((workerW = FindWindowExW(nullptr, workerW, L"WorkerW", nullptr))) {
            lv = findPair(workerW, &defView);
            if (lv) break;
        }
    }
    if (!lv || !defView) return;

    // Subclass the list-view
    g_desktopLV  = lv;
    g_origLVProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(lv, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(DesktopLVSubclassProc)));

    // Subclass the DefView parent to catch NM_CUSTOMDRAW
    g_defView         = defView;
    g_origDefViewProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(defView, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(DefViewSubclassProc)));

    InvalidateRect(lv, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Hook: DrawTextW  – suppress label text during desktop paint
// ---------------------------------------------------------------------------
using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
DrawTextW_t DrawTextW_Original;

int WINAPI DrawTextW_Hook(HDC hdc, LPCWSTR lpchText, int cchText,
                          LPRECT lprc, UINT format) {
    if (g_suppressDesktopText)
        return 0;
    return DrawTextW_Original(hdc, lpchText, cchText, lprc, format);
}

// ---------------------------------------------------------------------------
// Hook: DrawTextExW
// ---------------------------------------------------------------------------
using DrawTextExW_t = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT,
                                   LPDRAWTEXTPARAMS);
DrawTextExW_t DrawTextExW_Original;

int WINAPI DrawTextExW_Hook(HDC hdc, LPWSTR lpchText, int cchText,
                            LPRECT lprc, UINT format,
                            LPDRAWTEXTPARAMS lpdtp) {
    if (g_suppressDesktopText)
        return 0;
    return DrawTextExW_Original(hdc, lpchText, cchText, lprc, format, lpdtp);
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    Wh_Log(L"desktop-icon-hide-labels: ModInit v1.1");

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DrawTextW),
            reinterpret_cast<void*>(DrawTextW_Hook),
            reinterpret_cast<void**>(&DrawTextW_Original))) {
        Wh_Log(L"Failed to hook DrawTextW");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DrawTextExW),
            reinterpret_cast<void*>(DrawTextExW_Hook),
            reinterpret_cast<void**>(&DrawTextExW_Original))) {
        Wh_Log(L"Failed to hook DrawTextExW");
        return FALSE;
    }

    TrySubclassDesktopWindows();
    return TRUE;
}

void Wh_ModAfterInit() {
    TrySubclassDesktopWindows();
}

void Wh_ModUninit() {
    Wh_Log(L"desktop-icon-hide-labels: ModUninit");

    if (g_defView && g_origDefViewProc) {
        SetWindowLongPtrW(g_defView, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origDefViewProc));
        g_defView         = nullptr;
        g_origDefViewProc = nullptr;
    }

    if (g_desktopLV && g_origLVProc) {
        SetWindowLongPtrW(g_desktopLV, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origLVProc));
        InvalidateRect(g_desktopLV, nullptr, TRUE);
        g_desktopLV  = nullptr;
        g_origLVProc = nullptr;
    }
}