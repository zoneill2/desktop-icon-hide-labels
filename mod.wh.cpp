// ==WindhawkMod==
// @id              desktop-icon-hide-labels
// @name            Hide Desktop Icon Labels
// @description     Hides desktop icon labels and makes the selection highlight a uniform square centered on the icon.
// @version         2.1.0
// @author          zoneill
// @github          https://github.com/zoneill2
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hide Desktop Icon Labels

Hides the text labels beneath desktop icons and resizes the
selection/focus highlight rectangle so it wraps only the icon image,
not the label area below it.

Text is suppressed by intercepting DrawTextW during desktop ListView
paint cycles. The highlight is resized by adjusting FillRect and BitBlt
calls that cover the full icon cell, shrinking them by the computed
label height (iconOffsetY).
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

static volatile bool g_inDesktopPaint = false;
static volatile int  g_iconOffsetY    = 0;

// Queried icon dimensions (from the ListView's image list).
static int g_iconW = 0;
static int g_iconH = 0;

// Padding around the icon for the square highlight.
static const int HIGHLIGHT_PAD = 5;

// The computed square highlight side length: max(iconW, iconH) + 2*PAD
static int g_highlightSide = 0;

// Track the current item's full cell rect from CUSTOMDRAW so we can
// identify which FillRect/BitBlt calls correspond to the full cell
// vs the text strip.
static RECT g_currentCellRect = {};
static bool g_haveItemRect    = false;

// The desired square highlight rect for the current item, centered
// on the icon within the cell.
static RECT g_squareHighlight = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void QueryIconSize(HWND lv) {
    // Ask the ListView for its large-icon image list and read its icon size.
    HIMAGELIST hIml = reinterpret_cast<HIMAGELIST>(
        CallWindowProcW(g_origLVProc, lv, LVM_GETIMAGELIST, LVSIL_NORMAL, 0));
    if (hIml) {
        // Dynamically resolve ImageList_GetIconSize to avoid comctl32.lib dependency.
        using ImageList_GetIconSize_t = BOOL(WINAPI*)(HIMAGELIST, int*, int*);
        static ImageList_GetIconSize_t pFunc = nullptr;
        if (!pFunc) {
            HMODULE hComctl = GetModuleHandleW(L"comctl32.dll");
            if (hComctl)
                pFunc = reinterpret_cast<ImageList_GetIconSize_t>(
                    GetProcAddress(hComctl, "ImageList_GetIconSize"));
        }
        if (pFunc) {
            int cx = 0, cy = 0;
            if (pFunc(hIml, &cx, &cy)) {
                g_iconW = cx;
                g_iconH = cy;
            }
        }
    }
    // Fallback if image list query failed.
    if (g_iconW <= 0) g_iconW = 48;
    if (g_iconH <= 0) g_iconH = 48;

    int maxDim = (g_iconW > g_iconH) ? g_iconW : g_iconH;
    g_highlightSide = maxDim + 2 * HIGHLIGHT_PAD;
}

static int ComputeLabelHeight(HWND lv) {
    RECT rcLabel  = { LVIR_LABEL  };
    RECT rcBounds = { LVIR_BOUNDS };
    if (!CallWindowProcW(g_origLVProc, lv, LVM_GETITEMRECT, 0,
                         reinterpret_cast<LPARAM>(&rcLabel)))  return 0;
    if (!CallWindowProcW(g_origLVProc, lv, LVM_GETITEMRECT, 0,
                         reinterpret_cast<LPARAM>(&rcBounds))) return 0;
    int h = rcBounds.bottom - rcLabel.top;
    return h > 0 ? h : 0;
}

// Compute the centered square highlight rect for an item given its
// CUSTOMDRAW cell rect.  The icon is horizontally centered in the cell
// and sits at the top; we want a square centered on the icon.
static RECT ComputeSquareHighlight(const RECT& cell) {
    int cellW  = cell.right  - cell.left;
    int cellH  = cell.bottom - cell.top;
    // The icon area is the cell minus the label height at the bottom.
    int iconAreaH = cellH - g_iconOffsetY;
    if (iconAreaH < 0) iconAreaH = cellH;

    // Center of the icon area.
    int cx = cell.left + cellW / 2;
    int cy = cell.top  + iconAreaH / 2;

    int half = g_highlightSide / 2;
    RECT sq;
    sq.left   = cx - half;
    sq.top    = cy - half;
    sq.right  = cx + half;
    sq.bottom = cy + half;
    return sq;
}

// Check if a rect roughly matches the current cell rect dimensions.
static bool IsFullCellRect(const RECT* r) {
    if (!g_haveItemRect || !r) return false;
    int cellW = g_currentCellRect.right  - g_currentCellRect.left;
    int cellH = g_currentCellRect.bottom - g_currentCellRect.top;
    int rW = r->right  - r->left;
    int rH = r->bottom - r->top;
    return (rW >= cellW - 4 && rH >= cellH - 4 &&
            rW <= cellW + 20 && rH <= cellH + 20);
}

// Check if dimensions match the full cell (for BitBlt).
static bool IsFullCellBlit(int w, int h) {
    if (!g_haveItemRect) return false;
    int cellW = g_currentCellRect.right  - g_currentCellRect.left;
    int cellH = g_currentCellRect.bottom - g_currentCellRect.top;
    return (w >= cellW - 4 && h >= cellH - 4 &&
            w <= cellW + 20 && h <= cellH + 20);
}

// Check if this is a text-strip operation (small, height ~ iconOffsetY).
static bool IsTextStripRect(int w, int h) {
    return g_iconOffsetY > 0 && h > 0 && h <= g_iconOffsetY + 4 && w < 200;
}

// ---------------------------------------------------------------------------
// Hook: FillRect
//   - Full cell rect: shrink height by iconOffsetY (shrink highlight)
//   - Text strip rect: suppress entirely (no label background)
// ---------------------------------------------------------------------------
using FillRect_t = int(WINAPI*)(HDC, const RECT*, HBRUSH);
FillRect_t FillRect_Original;

static void* GetFillRectAddr() {
    HMODULE h = LoadLibraryW(L"user32.dll");
    return h ? reinterpret_cast<void*>(GetProcAddress(h, "FillRect")) : nullptr;
}

int WINAPI FillRect_Hook(HDC hdc, const RECT* lprc, HBRUSH hbr) {
    if (g_inDesktopPaint && lprc && g_iconOffsetY > 0 && g_haveItemRect) {
        int w = lprc->right  - lprc->left;
        int h = lprc->bottom - lprc->top;

        // Suppress text-strip fills entirely.
        if (IsTextStripRect(w, h)) {
            return 1;
        }

        // Replace full-cell fills with the square highlight.
        if (IsFullCellRect(lprc)) {
            return FillRect_Original(hdc, &g_squareHighlight, hbr);
        }
    }
    return FillRect_Original(hdc, lprc, hbr);
}

// ---------------------------------------------------------------------------
// Hook: BitBlt
//   - Full cell blit: shrink h by iconOffsetY
//   - Text strip blit: suppress entirely
// ---------------------------------------------------------------------------
using BitBlt_t = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
BitBlt_t BitBlt_Original;

static void* GetBitBltAddr() {
    HMODULE h = LoadLibraryW(L"gdi32.dll");
    return h ? reinterpret_cast<void*>(GetProcAddress(h, "BitBlt")) : nullptr;
}

BOOL WINAPI BitBlt_Hook(HDC hdcDest, int x, int y, int w, int h,
                         HDC hdcSrc, int x1, int y1, DWORD rop) {
    if (g_inDesktopPaint && g_iconOffsetY > 0 && g_haveItemRect) {
        // Suppress text-strip blits.
        if (IsTextStripRect(w, h)) {
            return TRUE;
        }

        // Replace full-cell blits with the square highlight region.
        if (IsFullCellBlit(w, h)) {
            int sqW = g_squareHighlight.right  - g_squareHighlight.left;
            int sqH = g_squareHighlight.bottom - g_squareHighlight.top;
            if (sqW > 0 && sqH > 0) {
                return BitBlt_Original(hdcDest,
                    g_squareHighlight.left, g_squareHighlight.top,
                    sqW, sqH,
                    hdcSrc,
                    g_squareHighlight.left, g_squareHighlight.top,
                    rop);
            }
            return TRUE;
        }
    }
    return BitBlt_Original(hdcDest, x, y, w, h, hdcSrc, x1, y1, rop);
}

// ---------------------------------------------------------------------------
// Hook: DrawTextW — suppress all text during desktop paint
// ---------------------------------------------------------------------------
using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
DrawTextW_t DrawTextW_Original;

int WINAPI DrawTextW_Hook(HDC hdc, LPCWSTR lpchText, int cchText,
                          LPRECT lprc, UINT format) {
    if (g_inDesktopPaint) {
        // Suppress all text drawing. Return a plausible height so
        // callers doing DT_CALCRECT get a non-zero value.
        return g_iconOffsetY > 0 ? g_iconOffsetY : 16;
    }
    return DrawTextW_Original(hdc, lpchText, cchText, lprc, format);
}

// ---------------------------------------------------------------------------
// Desktop SysListView32 subclass
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DesktopLVSubclassProc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        g_iconOffsetY    = ComputeLabelHeight(hwnd);
        QueryIconSize(hwnd);
        g_haveItemRect   = false;
        g_inDesktopPaint = true;
        LRESULT r = CallWindowProcW(g_origLVProc, hwnd, msg, wParam, lParam);
        g_inDesktopPaint = false;
        g_haveItemRect   = false;
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
// DefView subclass — intercept NM_CUSTOMDRAW to track item rects
// and shrink the highlight at ITEMPREPAINT.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DefViewSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NOTIFY && g_desktopLV) {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->hwndFrom == g_desktopLV && hdr->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;

            case CDDS_ITEMPREPAINT: {
                // Capture the original cell rect for matching in hooks.
                g_currentCellRect = cd->nmcd.rc;
                g_haveItemRect    = true;

                // Compute the centered square highlight for this item.
                g_squareHighlight = ComputeSquareHighlight(cd->nmcd.rc);

                // Override the CUSTOMDRAW rc to the square so the
                // ListView draws the highlight/focus rect as a square
                // centered on the icon.
                cd->nmcd.rc = g_squareHighlight;

                return CDRF_NOTIFYPOSTPAINT;
            }

            case CDDS_ITEMPOSTPAINT:
                g_haveItemRect = false;
                return CDRF_DODEFAULT;

            default:
                break;
            }
        }
    }
    return CallWindowProcW(g_origDefViewProc, hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Find and subclass
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

    Wh_Log(L"Subclassed lv=%p defView=%p", lv, defView);
    InvalidateRect(lv, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    Wh_Log(L"desktop-icon-hide-labels v2.1.0 init");

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DrawTextW),
            reinterpret_cast<void*>(DrawTextW_Hook),
            reinterpret_cast<void**>(&DrawTextW_Original))) {
        Wh_Log(L"Failed to hook DrawTextW");
        return FALSE;
    }

    void* pFillRect = GetFillRectAddr();
    if (pFillRect) {
        Wh_SetFunctionHook(pFillRect,
            reinterpret_cast<void*>(FillRect_Hook),
            reinterpret_cast<void**>(&FillRect_Original));
    }

    void* pBitBlt = GetBitBltAddr();
    if (pBitBlt) {
        Wh_SetFunctionHook(pBitBlt,
            reinterpret_cast<void*>(BitBlt_Hook),
            reinterpret_cast<void**>(&BitBlt_Original));
    }

    TrySubclassDesktopWindows();
    return TRUE;
}

void Wh_ModAfterInit() {
    TrySubclassDesktopWindows();
}

void Wh_ModUninit() {
    Wh_Log(L"desktop-icon-hide-labels: uninit");

    if (g_defView && g_origDefViewProc) {
        SetWindowLongPtrW(g_defView, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origDefViewProc));
        g_defView = nullptr;
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