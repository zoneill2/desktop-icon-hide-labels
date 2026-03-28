// ==WindhawkMod==
// @id              desktop-icon-hide-labels
// @name            Hide Desktop Icon Labels
// @description     Hides the text labels beneath desktop icons and centers the icon within its cell.
// @version         1.3.0
// @author          YourName
// @github          https://github.com/YourName
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hide Desktop Icon Labels

Hides the text name labels displayed beneath desktop icons, leaving only the
icons themselves visible. The icon is vertically centered within the full item
cell so the selection highlight and click hit-testing both wrap the icon.

Tooltips still work on hover, and the actual file/shortcut names are **not**
changed in any way.

## Notes

- Works on Windows 10 and Windows 11.
- The mod targets `explorer.exe` only.
- Right-click the desktop and choose Refresh if icons don't update immediately.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <commctrl.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HWND    g_desktopLV       = nullptr;
static WNDPROC g_origLVProc      = nullptr;
static HWND    g_defView         = nullptr;
static WNDPROC g_origDefViewProc = nullptr;

// Set to true while the desktop LV is executing a WM_PAINT / WM_PRINTCLIENT.
// Used to gate both the DrawText suppression and the ImageList_DrawEx offset.
thread_local bool g_inDesktopPaint = false;

// How many pixels to push the icon downward so it sits in the vertical center
// of the full cell (icon + label area). Calculated fresh each paint from the
// actual item geometry so it works at any DPI / icon size.
thread_local int g_iconOffsetY = 0;

// ---------------------------------------------------------------------------
// Compute the vertical offset needed to center the icon in the full cell.
// The desktop LV cell normally looks like:
//
//   [  top padding  ]
//   [    ICON       ]   ← LVIR_ICON top/bottom
//   [  gap          ]
//   [   Label text  ]   ← LVIR_LABEL top/bottom
//   [  bottom pad   ]
//
// With labels hidden we want the icon centered in the whole LVIR_BOUNDS rect.
// ---------------------------------------------------------------------------
static int ComputeIconOffsetY(HWND lv) {
    if (!lv) return 0;

    // Use item 0 as the reference — all items have the same geometry.
    RECT rcBounds = { LVIR_BOUNDS };
    RECT rcIcon   = { LVIR_ICON   };

    // Send directly to the original proc so we get the unpatched rects.
    if (!CallWindowProcW(g_origLVProc, lv, LVM_GETITEMRECT, 0,
                         reinterpret_cast<LPARAM>(&rcBounds))) return 0;
    if (!CallWindowProcW(g_origLVProc, lv, LVM_GETITEMRECT, 0,
                         reinterpret_cast<LPARAM>(&rcIcon)))   return 0;

    int cellH  = rcBounds.bottom - rcBounds.top;
    int iconH  = rcIcon.bottom   - rcIcon.top;
    int iconTopInCell = rcIcon.top - rcBounds.top;  // current distance from top

    // Where we WANT the icon top to be (centered):
    int wantedTopInCell = (cellH - iconH) / 2;

    return wantedTopInCell - iconTopInCell;  // positive = shift down
}

// ---------------------------------------------------------------------------
// Hook: ImageList_DrawEx
// comctl32 calls this to stamp the icon bitmap at a specific (x,y).
// We intercept it and shift y by g_iconOffsetY when inside a desktop paint.
// Loaded dynamically to avoid a comctl32.lib linker dependency.
// ---------------------------------------------------------------------------
using ImageList_DrawEx_t = BOOL(WINAPI*)(HIMAGELIST, int, HDC,
                                          int, int, int, int,
                                          COLORREF, COLORREF, UINT);
ImageList_DrawEx_t ImageList_DrawEx_Original;

static void* GetImageList_DrawExAddr() {
    HMODULE hComctl = LoadLibraryW(L"comctl32.dll");
    if (!hComctl) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(hComctl, "ImageList_DrawEx"));
}

BOOL WINAPI ImageList_DrawEx_Hook(HIMAGELIST himl, int i, HDC hdcDst,
                                   int x, int y, int dx, int dy,
                                   COLORREF rgbBk, COLORREF rgbFg, UINT fStyle) {
    if (g_inDesktopPaint) {
        y += g_iconOffsetY;
    }
    return ImageList_DrawEx_Original(himl, i, hdcDst,
                                     x, y, dx, dy, rgbBk, rgbFg, fStyle);
}

// ---------------------------------------------------------------------------
// Hook: DrawTextW — suppress label text during desktop paint
// ---------------------------------------------------------------------------
using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
DrawTextW_t DrawTextW_Original;

int WINAPI DrawTextW_Hook(HDC hdc, LPCWSTR lpchText, int cchText,
                          LPRECT lprc, UINT format) {
    if (g_inDesktopPaint) return 0;
    return DrawTextW_Original(hdc, lpchText, cchText, lprc, format);
}

// ---------------------------------------------------------------------------
// Hook: DrawTextExW — suppress label text during desktop paint
// ---------------------------------------------------------------------------
using DrawTextExW_t = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT,
                                   LPDRAWTEXTPARAMS);
DrawTextExW_t DrawTextExW_Original;

int WINAPI DrawTextExW_Hook(HDC hdc, LPWSTR lpchText, int cchText,
                            LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp) {
    if (g_inDesktopPaint) return 0;
    return DrawTextExW_Original(hdc, lpchText, cchText, lprc, format, lpdtp);
}

// ---------------------------------------------------------------------------
// Desktop SysListView32 subclass proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DesktopLVSubclassProc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        // Compute the offset once per paint pass (uses item 0 geometry).
        g_iconOffsetY    = ComputeIconOffsetY(hwnd);
        g_inDesktopPaint = true;
        LRESULT r = CallWindowProcW(g_origLVProc, hwnd, msg, wParam, lParam);
        g_inDesktopPaint = false;
        return r;
    }

    // Shrink BOUNDS / SELECTBOUNDS to the re-centered icon rect so that
    // the selection highlight box and hit-test match the visual icon position.
    case LVM_GETITEMRECT: {
        RECT* prc  = reinterpret_cast<RECT*>(lParam);
        if (!prc) break;
        int code = prc->left;
        int item = static_cast<int>(wParam);

        // Get un-patched rects from the real proc
        RECT rcBounds = { LVIR_BOUNDS };
        RECT rcIcon   = { LVIR_ICON   };
        LRESULT rb = CallWindowProcW(g_origLVProc, hwnd, LVM_GETITEMRECT,
                                     item, reinterpret_cast<LPARAM>(&rcBounds));
        LRESULT ri = CallWindowProcW(g_origLVProc, hwnd, LVM_GETITEMRECT,
                                     item, reinterpret_cast<LPARAM>(&rcIcon));
        if (!rb || !ri) break;

        int cellH  = rcBounds.bottom - rcBounds.top;
        int cellW  = rcBounds.right  - rcBounds.left;
        int iconH  = rcIcon.bottom   - rcIcon.top;
        int iconW  = rcIcon.right    - rcIcon.left;

        // Centered icon rect in screen coords
        int newTop  = rcBounds.top  + (cellH - iconH) / 2;
        int newLeft = rcBounds.left + (cellW - iconW) / 2;
        RECT rcCentered = { newLeft, newTop, newLeft + iconW, newTop + iconH };

        switch (code) {
        case LVIR_ICON:
        case LVIR_BOUNDS:
        case LVIR_SELECTBOUNDS:
            *prc = rcCentered;
            return TRUE;
        case LVIR_LABEL:
            // Zero-height rect — nothing to click or draw
            *prc = { rcCentered.left,  rcCentered.bottom,
                     rcCentered.right, rcCentered.bottom };
            return TRUE;
        }
        break;
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
// SHELLDLL_DefView subclass — only needed so NM_CUSTOMDRAW CDDS_PREPAINT
// returns CDRF_NOTIFYITEMDRAW (keeps custom draw pipeline alive) without
// us needing to do any manual drawing.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DefViewSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NOTIFY && g_desktopLV) {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->hwndFrom == g_desktopLV && hdr->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
        }
    }
    return CallWindowProcW(g_origDefViewProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Find desktop windows and install subclasses
// ---------------------------------------------------------------------------
static void TrySubclassDesktopWindows() {
    if (g_desktopLV) return;

    auto findPair = [](HWND parent, HWND* outDV) -> HWND {
        HWND dv = FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!dv) return nullptr;
        HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
        if (lv && outDV) *outDV = dv;
        return lv;
    };

    HWND defView = nullptr, lv = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) lv = findPair(progman, &defView);

    if (!lv) {
        HWND workerW = nullptr;
        while ((workerW = FindWindowExW(nullptr, workerW, L"WorkerW", nullptr))) {
            lv = findPair(workerW, &defView);
            if (lv) break;
        }
    }
    if (!lv || !defView) return;

    g_desktopLV  = lv;
    g_origLVProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(lv, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(DesktopLVSubclassProc)));

    g_defView         = defView;
    g_origDefViewProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(defView, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(DefViewSubclassProc)));

    InvalidateRect(lv, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    Wh_Log(L"desktop-icon-hide-labels: ModInit v1.3");

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DrawTextW),
            reinterpret_cast<void*>(DrawTextW_Hook),
            reinterpret_cast<void**>(&DrawTextW_Original))) {
        Wh_Log(L"Failed to hook DrawTextW"); return FALSE;
    }
    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DrawTextExW),
            reinterpret_cast<void*>(DrawTextExW_Hook),
            reinterpret_cast<void**>(&DrawTextExW_Original))) {
        Wh_Log(L"Failed to hook DrawTextExW"); return FALSE;
    }
    void* pImageList_DrawEx = GetImageList_DrawExAddr();
    if (!pImageList_DrawEx) {
        Wh_Log(L"Failed to find ImageList_DrawEx"); return FALSE;
    }
    if (!Wh_SetFunctionHook(
            pImageList_DrawEx,
            reinterpret_cast<void*>(ImageList_DrawEx_Hook),
            reinterpret_cast<void**>(&ImageList_DrawEx_Original))) {
        Wh_Log(L"Failed to hook ImageList_DrawEx"); return FALSE;
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
        g_defView = nullptr; g_origDefViewProc = nullptr;
    }
    if (g_desktopLV && g_origLVProc) {
        SetWindowLongPtrW(g_desktopLV, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origLVProc));
        InvalidateRect(g_desktopLV, nullptr, TRUE);
        g_desktopLV = nullptr; g_origLVProc = nullptr;
    }
}