/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <windows.h>
#include <windowsx.h>

#include "options.h"
#include "input/keycodes.h"
#include "osdep/resource.h"
#include "input/input.h"
#include "mp_msg.h"
#include "video_out.h"
#include "aspect.h"
#include "w32_common.h"
#include "mp_fifo.h"
#include "osdep/io.h"
#include "talloc.h"

#define WIN_ID_TO_HWND(x) ((HWND)(uint32_t)(x))

static const wchar_t classname[] = L"mplayer2";

static const struct mp_keymap vk_map[] = {
    // special keys
    {VK_ESCAPE, KEY_ESC}, {VK_BACK, KEY_BS}, {VK_TAB, KEY_TAB},
    {VK_RETURN, KEY_ENTER}, {VK_PAUSE, KEY_PAUSE}, {VK_SNAPSHOT, KEY_PRINT},

    // cursor keys
    {VK_LEFT, KEY_LEFT}, {VK_UP, KEY_UP}, {VK_RIGHT, KEY_RIGHT}, {VK_DOWN, KEY_DOWN},

    // navigation block
    {VK_INSERT, KEY_INSERT}, {VK_DELETE, KEY_DELETE}, {VK_HOME, KEY_HOME}, {VK_END, KEY_END},
    {VK_PRIOR, KEY_PAGE_UP}, {VK_NEXT, KEY_PAGE_DOWN},

    // F-keys
    {VK_F1, KEY_F+1}, {VK_F2, KEY_F+2}, {VK_F3, KEY_F+3}, {VK_F4, KEY_F+4},
    {VK_F5, KEY_F+5}, {VK_F6, KEY_F+6}, {VK_F7, KEY_F+7}, {VK_F8, KEY_F+8},
    {VK_F9, KEY_F+9}, {VK_F10, KEY_F+10}, {VK_F11, KEY_F+11}, {VK_F12, KEY_F+12},
    // numpad
    {VK_NUMPAD0, KEY_KP0}, {VK_NUMPAD1, KEY_KP1}, {VK_NUMPAD2, KEY_KP2},
    {VK_NUMPAD3, KEY_KP3}, {VK_NUMPAD4, KEY_KP4}, {VK_NUMPAD5, KEY_KP5},
    {VK_NUMPAD6, KEY_KP6}, {VK_NUMPAD7, KEY_KP7}, {VK_NUMPAD8, KEY_KP8},
    {VK_NUMPAD9, KEY_KP9}, {VK_DECIMAL, KEY_KPDEC},

    {0, 0}
};

static void add_window_borders(HWND hwnd, RECT *rc)
{
    AdjustWindowRect(rc, GetWindowLong(hwnd, GWL_STYLE), 0);
}

// basically a reverse AdjustWindowRect (win32 doesn't appear to have this)
static void subtract_window_borders(HWND hwnd, RECT *rc)
{
    RECT b = { 0, 0, 0, 0 };
    add_window_borders(hwnd, &b);
    rc->left -= b.left;
    rc->top -= b.top;
    rc->right -= b.right;
    rc->bottom -= b.bottom;
}

// turn a WMSZ_* input value in v into the border that should be resized
// returns: 0=left, 1=top, 2=right, 3=bottom, -1=undefined
static int get_resize_border(int v) {
    switch (v) {
    case WMSZ_LEFT: return 3;
    case WMSZ_TOP: return 2;
    case WMSZ_RIGHT: return 3;
    case WMSZ_BOTTOM: return 2;
    case WMSZ_TOPLEFT: return 1;
    case WMSZ_TOPRIGHT: return 1;
    case WMSZ_BOTTOMLEFT: return 3;
    case WMSZ_BOTTOMRIGHT: return 3;
    default: return -1;
    }
}

static bool key_state(struct vo *vo, int vk)
{
    return GetKeyState(vk) & 0x8000;
}

static int mod_state(struct vo *vo)
{
    int res = 0;
    if (key_state(vo, VK_CONTROL))
        res |= KEY_MODIFIER_CTRL;
    if (key_state(vo, VK_SHIFT))
        res |= KEY_MODIFIER_SHIFT;
    if (key_state(vo, VK_MENU))
        res |= KEY_MODIFIER_ALT;
    return res;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        CREATESTRUCT *cs = (void*)lParam;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    struct vo *vo = (void*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    // message before WM_NCCREATE, pray to Raymond Chen that it's not important
    if (!vo)
        return DefWindowProcW(hWnd, message, wParam, lParam);
    struct vo_w32_state *w32 = vo->w32;

    switch (message) {
        case WM_ERASEBKGND: // no need to erase background seperately
            return 1;
        case WM_PAINT:
            w32->event_flags |= VO_EVENT_EXPOSE;
            break;
        case WM_MOVE: {
            w32->event_flags |= VO_EVENT_MOVE;
            POINT p = {0};
            ClientToScreen(w32->window, &p);
            w32->window_x = p.x;
            w32->window_y = p.y;
            mp_msg(MSGT_VO, MSGL_V, "[vo] move window: %d:%d\n",
                   w32->window_x, w32->window_y);
            break;
        }
        case WM_SIZE: {
            w32->event_flags |= VO_EVENT_RESIZE;
            RECT r;
            GetClientRect(w32->window, &r);
            vo->dwidth = r.right;
            vo->dheight = r.bottom;
            mp_msg(MSGT_VO, MSGL_V, "[vo] resize window: %d:%d\n",
                   vo->dwidth, vo->dheight);
            break;
        }
        case WM_SIZING:
            if (vo_keepaspect && !vo_fs && WinID < 0) {
                RECT *rc = (RECT*)lParam;
                // get client area of the windows if it had the rect rc
                // (subtracting the window borders)
                RECT r = *rc;
                subtract_window_borders(w32->window, &r);
                int c_w = r.right - r.left, c_h = r.bottom - r.top;
                float aspect = vo->aspdat.asp;
                int d_w = c_h * aspect - c_w;
                int d_h = c_w / aspect - c_h;
                int d_corners[4] = { d_w, d_h, -d_w, -d_h };
                int corners[4] = { rc->left, rc->top, rc->right, rc->bottom };
                int corner = get_resize_border(wParam);
                if (corner >= 0)
                    corners[corner] -= d_corners[corner];
                *rc = (RECT) { corners[0], corners[1], corners[2], corners[3] };
                return TRUE;
            }
            break;
        case WM_CLOSE:
            mplayer_put_key(vo->key_fifo, KEY_CLOSE_WIN);
            break;
        case WM_SYSCOMMAND:
            switch (wParam) {
                case SC_SCREENSAVE:
                case SC_MONITORPOWER:
                    mp_msg(MSGT_VO, MSGL_V, "vo: win32: killing screensaver\n");
                    return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int mpkey = lookup_keymap_table(vk_map, wParam);
            if (mpkey)
                mplayer_put_key(vo->key_fifo, mpkey | mod_state(vo));
            if (wParam == VK_F10)
                return 0;
            break;
        }
        case WM_CHAR:
        case WM_SYSCHAR: {
            int mods = mod_state(vo);
            int code = wParam;
            // Windows enables Ctrl+Alt when AltGr (VK_RMENU) is pressed.
            // E.g. AltGr+9 on a German keyboard would yield Ctrl+Alt+[
            // Warning: wine handles this differently. Don't test this on wine!
            if (key_state(vo, VK_RMENU))
                mods &= ~(KEY_MODIFIER_CTRL | KEY_MODIFIER_ALT);
            // Apparently Ctrl+A to Ctrl+Z is special cased, and produces
            // character codes from 1-26. Work it around.
            // Also, enter/return (including the keypad variant) and CTRL+J both
            // map to wParam==10. As a workaround, check VK_RETURN to
            // distinguish these two key combinations.
            if ((mods & KEY_MODIFIER_CTRL) && code >= 1 && code <= 26
                && !key_state(vo, VK_RETURN))
                code = code - 1 + (mods & KEY_MODIFIER_SHIFT ? 'A' : 'a');
            if (code >= 32 && code < (1<<21)) {
                mplayer_put_key(vo->key_fifo, code | mods);
                // At least with Alt+char, not calling DefWindowProcW stops
                // Windows from emitting a beep.
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            if (!vo_nomouse_input && (vo_fs || (wParam & MK_CONTROL))) {
                mplayer_put_key(vo->key_fifo, MOUSE_BTN0 | mod_state(vo));
                break;
            }
            if (!vo_fs) {
                ReleaseCapture();
                SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            break;
        case WM_MBUTTONDOWN:
            if (!vo_nomouse_input)
                mplayer_put_key(vo->key_fifo, MOUSE_BTN1 | mod_state(vo));
            break;
        case WM_RBUTTONDOWN:
            if (!vo_nomouse_input)
                mplayer_put_key(vo->key_fifo, MOUSE_BTN2 | mod_state(vo));
            break;
        case WM_MOUSEMOVE:
            vo_mouse_movement(vo, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            break;
        case WM_MOUSEWHEEL:
            if (!vo_nomouse_input) {
                int x = GET_WHEEL_DELTA_WPARAM(wParam);
                if (x > 0)
                    mplayer_put_key(vo->key_fifo, MOUSE_BTN3 | mod_state(vo));
                else
                    mplayer_put_key(vo->key_fifo, MOUSE_BTN4 | mod_state(vo));
            }
            break;
        case WM_XBUTTONDOWN:
            if (!vo_nomouse_input) {
                int x = HIWORD(wParam);
                if (x == 1)
                    mplayer_put_key(vo->key_fifo, MOUSE_BTN5 | mod_state(vo));
                else // if (x == 2)
                    mplayer_put_key(vo->key_fifo, MOUSE_BTN6 | mod_state(vo));
            }
            break;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

/**
 * \brief Dispatch incoming window events and handle them.
 *
 * This function should be placed inside libvo's function "check_events".
 *
 * \return int with these flags possibly set, take care to handle in the right order
 *         if it matters in your driver:
 *
 * VO_EVENT_RESIZE = The window was resized. If necessary reinit your
 *                   driver render context accordingly.
 * VO_EVENT_EXPOSE = The window was exposed. Call e.g. flip_frame() to redraw
 *                   the window if the movie is paused.
 */
int vo_w32_check_events(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    MSG msg;
    w32->event_flags = 0;
    while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (WinID >= 0) {
        BOOL res;
        RECT r;
        POINT p;
        res = GetClientRect(w32->window, &r);
        if (res && (r.right != vo->dwidth || r.bottom != vo->dheight)) {
            vo->dwidth = r.right; vo->dheight = r.bottom;
            w32->event_flags |= VO_EVENT_RESIZE;
        }
        p.x = 0; p.y = 0;
        ClientToScreen(w32->window, &p);
        if (p.x != w32->window_x || p.y != w32->window_y) {
            w32->window_x = p.x; w32->window_y = p.y;
            w32->event_flags |= VO_EVENT_MOVE;
        }
        res = GetClientRect(WIN_ID_TO_HWND(WinID), &r);
        if (res && (r.right != vo->dwidth || r.bottom != vo->dheight))
            MoveWindow(w32->window, 0, 0, r.right, r.bottom, FALSE);
        if (!IsWindow(WIN_ID_TO_HWND(WinID)))
            // Window has probably been closed, e.g. due to program crash
            mplayer_put_key(vo->key_fifo, KEY_CLOSE_WIN);
    }

    return w32->event_flags;
}

static BOOL CALLBACK mon_enum(HMONITOR hmon, HDC hdc, LPRECT r, LPARAM p)
{
    struct vo *vo = (void*)p;
    struct vo_w32_state *w32 = vo->w32;
    // this defaults to the last screen if specified number does not exist
    xinerama_x = r->left;
    xinerama_y = r->top;
    vo->opts->vo_screenwidth = r->right - r->left;
    vo->opts->vo_screenheight = r->bottom - r->top;
    if (w32->mon_cnt == xinerama_screen)
        return FALSE;
    w32->mon_cnt++;
    return TRUE;
}

/**
 * \brief Update screen information.
 *
 * This function should be called in libvo's "control" callback
 * with parameter VOCTRL_UPDATE_SCREENINFO.
 * Note that this also enables the new API where geometry and aspect
 * calculations are done in video_out.c:config_video_out
 *
 * Global libvo variables changed:
 * xinerama_x
 * xinerama_y
 * vo_screenwidth
 * vo_screenheight
 */
void w32_update_xinerama_info(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    xinerama_x = xinerama_y = 0;
    if (xinerama_screen < -1) {
        int tmp;
        xinerama_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        xinerama_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        tmp = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        if (tmp) vo->opts->vo_screenwidth = tmp;
        tmp = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (tmp) vo->opts->vo_screenheight = tmp;
    } else if (xinerama_screen == -1) {
        MONITORINFO mi;
        HMONITOR m = MonitorFromWindow(w32->window, MONITOR_DEFAULTTOPRIMARY);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(m, &mi);
        xinerama_x = mi.rcMonitor.left;
        xinerama_y = mi.rcMonitor.top;
        vo->opts->vo_screenwidth = mi.rcMonitor.right - mi.rcMonitor.left;
        vo->opts->vo_screenheight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else if (xinerama_screen > 0) {
        w32->mon_cnt = 0;
        EnumDisplayMonitors(NULL, NULL, mon_enum, (LONG_PTR)vo);
    }
    aspect_save_screenres(vo, vo->opts->vo_screenwidth,
                          vo->opts->vo_screenheight);
}

static void updateScreenProperties(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    DEVMODE dm;
    dm.dmSize = sizeof dm;
    dm.dmDriverExtra = 0;
    dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (!EnumDisplaySettings(0, ENUM_CURRENT_SETTINGS, &dm)) {
        mp_msg(MSGT_VO, MSGL_ERR,
               "vo: win32: unable to enumerate display settings!\n");
        return;
    }

    vo->opts->vo_screenwidth = dm.dmPelsWidth;
    vo->opts->vo_screenheight = dm.dmPelsHeight;
    w32->depthonscreen = dm.dmBitsPerPel;
    w32_update_xinerama_info(vo);
}

static void changeMode(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    DEVMODE dm;
    dm.dmSize = sizeof dm;
    dm.dmDriverExtra = 0;

    dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmBitsPerPel = w32->depthonscreen;
    dm.dmPelsWidth = vo->opts->vo_screenwidth;
    dm.dmPelsHeight = vo->opts->vo_screenheight;

    if (w32->vm) {
        int bestMode = -1;
        int bestScore = INT_MAX;
        int i;
        for (i = 0; EnumDisplaySettings(0, i, &dm); ++i) {
            int score = (dm.dmPelsWidth - w32->o_dwidth)
                        * (dm.dmPelsHeight - w32->o_dheight);
            if (dm.dmBitsPerPel != w32->depthonscreen
                || dm.dmPelsWidth < w32->o_dwidth
                || dm.dmPelsHeight < w32->o_dheight)
                continue;

            if (score < bestScore) {
                bestScore = score;
                bestMode = i;
            }
        }

        if (bestMode != -1)
            EnumDisplaySettings(0, bestMode, &dm);

        ChangeDisplaySettings(&dm, CDS_FULLSCREEN);
    }
}

static void resetMode(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    if (w32->vm)
        ChangeDisplaySettings(0, 0);
}

static DWORD update_style(struct vo *vo, DWORD style)
{
    const DWORD NO_FRAME = WS_POPUP;
    const DWORD FRAME = WS_OVERLAPPEDWINDOW | WS_SIZEBOX;
    style &= ~(NO_FRAME | FRAME);
    style |= (vo_border && !vo_fs) ? FRAME : NO_FRAME;
    return style;
}

// Update the window title, position, size, and border style from vo_* values.
static int reinit_window_state(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    HWND layer = HWND_NOTOPMOST;
    RECT r;

    if (WinID >= 0)
        return 1;

    wchar_t *title = mp_from_utf8(NULL, vo_get_window_title(vo));
    SetWindowTextW(w32->window, title);
    talloc_free(title);

    bool toggle_fs = w32->current_fs != vo_fs;
    w32->current_fs = vo_fs;

    DWORD style = update_style(vo, GetWindowLong(w32->window, GWL_STYLE));

    if (vo_fs || vo->opts->vo_ontop)
        layer = HWND_TOPMOST;

    // xxx not sure if this can trigger any unwanted messages (WM_MOVE/WM_SIZE)
    if (vo_fs) {
        changeMode(vo);
        while (ShowCursor(0) >= 0) /**/ ;
    } else {
        resetMode(vo);
        while (ShowCursor(1) < 0) /**/ ;
    }
    updateScreenProperties(vo);

    if (vo_fs) {
        // Save window position and size when switching to fullscreen.
        if (toggle_fs) {
            w32->prev_width = vo->dwidth;
            w32->prev_height = vo->dheight;
            w32->prev_x = w32->window_x;
            w32->prev_y = w32->window_y;
            mp_msg(MSGT_VO, MSGL_V, "[vo] save window bounds: %d:%d:%d:%d\n",
                   w32->prev_x, w32->prev_y, w32->prev_width, w32->prev_height);
        }
        vo->dwidth = vo->opts->vo_screenwidth;
        vo->dheight = vo->opts->vo_screenheight;
        w32->window_x = xinerama_x;
        w32->window_y = xinerama_y;
    } else {
        if (toggle_fs) {
            // Restore window position and size when switching from fullscreen.
            mp_msg(MSGT_VO, MSGL_V, "[vo] restore window bounds: %d:%d:%d:%d\n",
                   w32->prev_x, w32->prev_y, w32->prev_width, w32->prev_height);
            vo->dwidth = w32->prev_width;
            vo->dheight = w32->prev_height;
            w32->window_x = w32->prev_x;
            w32->window_y = w32->prev_y;
        }
    }

    r.left = w32->window_x;
    r.right = r.left + vo->dwidth;
    r.top = w32->window_y;
    r.bottom = r.top + vo->dheight;

    SetWindowLong(w32->window, GWL_STYLE, style);
    add_window_borders(w32->window, &r);

    mp_msg(MSGT_VO, MSGL_V, "[vo] reset window bounds: %ld:%ld:%ld:%ld\n",
           r.left, r.top, r.right - r.left, r.bottom - r.top);

    SetWindowPos(w32->window, layer, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED);
    // For some reason, moving SWP_SHOWWINDOW to a second call works better
    // with wine: returning from fullscreen doesn't cause a bogus resize to
    // screen size.
    // It's not needed on Windows XP or wine with a virtual desktop.
    // It doesn't seem to have any negative effects.
    SetWindowPos(w32->window, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

    return 1;
}

/**
 * \brief Configure and show window on the screen.
 *
 * This function should be called in libvo's "config" callback.
 * It configures a window and shows it on the screen.
 *
 * \return 1 - Success, 0 - Failure
 */
int vo_w32_config(struct vo *vo, uint32_t width, uint32_t height,
                  uint32_t flags)
{
    struct vo_w32_state *w32 = vo->w32;
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    HDC vo_hdc = vo_w32_get_dc(vo, w32->window);

    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    if (flags & VOFLAG_STEREO)
        pfd.dwFlags |= PFD_STEREO;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(vo_hdc, &pfd);
    if (!pf) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo: win32: unable to select a valid pixel format!\n");
        vo_w32_release_dc(vo, w32->window, vo_hdc);
        return 0;
    }

    SetPixelFormat(vo_hdc, pf, &pfd);
    vo_w32_release_dc(vo, w32->window, vo_hdc);

    // we already have a fully initialized window, so nothing needs to be done
    if (flags & VOFLAG_HIDDEN)
        return 1;

    bool reset_size = !(w32->o_dwidth == width && w32->o_dheight == height);

    w32->o_dwidth = width;
    w32->o_dheight = height;

    // the desired size is ignored in wid mode, it always matches the window size.
    if (WinID < 0) {
        if (w32->window_bounds_initialized) {
            // restore vo_dwidth/vo_dheight, which are reset against our will
            // in vo_config()
            RECT r;
            GetClientRect(w32->window, &r);
            vo->dwidth = r.right;
            vo->dheight = r.bottom;
        } else {
            // first vo_config call; vo_config() will always set vo_dx/dy so
            // that the window is centered on the screen, and this is the only
            // time we actually want to use vo_dy/dy (this is not sane, and
            // video_out.h should provide a function to query the initial
            // window position instead)
            w32->window_bounds_initialized = true;
            reset_size = true;
            w32->window_x = w32->prev_x = vo->dx;
            w32->window_y = w32->prev_y = vo->dy;
        }
        if (reset_size) {
            w32->prev_width = vo->dwidth = width;
            w32->prev_height = vo->dheight = height;
        }
    }

    vo_fs = flags & VOFLAG_FULLSCREEN;
    w32->vm = flags & VOFLAG_MODESWITCHING;
    return reinit_window_state(vo);
}

/**
 * \brief return the name of the selected device if it is indepedant
 * \return pointer to string, must be freed.
 */
static wchar_t *get_display_name(void)
{
    DISPLAY_DEVICEW disp;
    disp.cb = sizeof(disp);
    EnumDisplayDevicesW(NULL, vo_adapter_num, &disp, 0);
    if (disp.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
        return NULL;
    return wcsdup(disp.DeviceName);
}

/**
 * \brief Initialize w32_common framework.
 *
 * The first function that should be called from the w32_common framework.
 * It handles window creation on the screen with proper title and attributes.
 * It also initializes the framework's internal variables. The function should
 * be called after your own preinit initialization and you shouldn't do any
 * window management on your own.
 *
 * Global libvo variables changed:
 * vo_w32_window
 * vo_screenwidth
 * vo_screenheight
 *
 * \return 1 = Success, 0 = Failure
 */
int vo_w32_init(struct vo *vo)
{
    HICON mplayerIcon = NULL;
    HICON mplayerSmallIcon = NULL;
    struct vo_w32_state *w32 = vo->w32;

    if (w32 && w32->window)
        return 1;

    if (!w32)
        w32 = vo->w32 = talloc_zero(vo, struct vo_w32_state);

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    mplayerIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    mplayerSmallIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 16, 16, LR_SHARED);

    WNDCLASSEXW wcex = {
        .cbSize = sizeof wcex,
        .style = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = WndProc,
        .hInstance = hInstance,
        .hIcon = mplayerIcon,
        .hCursor = LoadCursor(0, IDC_ARROW),
        .lpszClassName = classname,
        .hIconSm = mplayerSmallIcon,
    };

    if (!RegisterClassExW(&wcex)) {
        mp_msg(MSGT_VO, MSGL_ERR,
               "vo: win32: unable to register window class!\n");
        return 0;
    }

    if (WinID >= 0) {
        RECT r;
        GetClientRect(WIN_ID_TO_HWND(WinID), &r);
        vo->dwidth = r.right; vo->dheight = r.bottom;
        w32->window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, classname,
                                      classname,
                                      WS_CHILD | WS_VISIBLE,
                                      0, 0, vo->dwidth, vo->dheight,
                                      WIN_ID_TO_HWND(WinID), 0, hInstance, vo);
    } else {
        w32->window = CreateWindowExW(0, classname,
                                      classname,
                                      update_style(vo, 0),
                                      CW_USEDEFAULT, 0, 100, 100,
                                      0, 0, hInstance, vo);
    }

    if (!w32->window) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo: win32: unable to create window!\n");
        return 0;
    }

    if (WinID >= 0)
        EnableWindow(w32->window, 0);

    w32->dev_hdc = 0;
    wchar_t *dev = get_display_name();
    if (dev) w32->dev_hdc = CreateDCW(dev, NULL, NULL, NULL);
    free(dev);
    updateScreenProperties(vo);

    mp_msg(MSGT_VO, MSGL_V, "vo: win32: running at %dx%d with depth %d\n",
           vo->opts->vo_screenwidth, vo->opts->vo_screenheight,
           w32->depthonscreen);

    return 1;
}

/**
 * \brief Toogle fullscreen / windowed mode.
 *
 * Should be called on VOCTRL_FULLSCREEN event. The window is
 * always resized during this call, so the rendering context
 * should be reinitialized with the new dimensions.
 * It is unspecified if vo_check_events will create a resize
 * event in addition or not.
 */

void vo_w32_fullscreen(struct vo *vo)
{
    vo_fs = !vo_fs;
    reinit_window_state(vo);
}

/**
 * \brief Toogle window border attribute.
 *
 * Should be called on VOCTRL_BORDER event.
 */
void vo_w32_border(struct vo *vo)
{
    vo_border = !vo_border;
    reinit_window_state(vo);
}

/**
 * \brief Toogle window ontop attribute.
 *
 * Should be called on VOCTRL_ONTOP event.
 */
void vo_w32_ontop(struct vo *vo)
{
    vo->opts->vo_ontop = !vo->opts->vo_ontop;
    reinit_window_state(vo);
}

/**
 * \brief Uninitialize w32_common framework.
 *
 * Should be called last in video driver's uninit function. First release
 * anything built on top of the created window e.g. rendering context inside
 * and call vo_w32_uninit at the end.
 */
void vo_w32_uninit(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    mp_msg(MSGT_VO, MSGL_V, "vo: win32: uninit\n");
    if (!w32)
        return;
    resetMode(vo);
    ShowCursor(1);
    if (w32->dev_hdc) DeleteDC(w32->dev_hdc);
    DestroyWindow(w32->window);
    UnregisterClassW(classname, 0);
    talloc_free(w32);
    vo->w32 = NULL;
}

/**
 * \brief get a device context to draw in
 *
 * \param wnd window the DC should belong to if it makes sense
 */
HDC vo_w32_get_dc(struct vo *vo, HWND wnd)
{
    struct vo_w32_state *w32 = vo->w32;
    if (w32->dev_hdc)
        return w32->dev_hdc;
    return GetDC(wnd);
}

/**
 * \brief release a device context
 *
 * \param wnd window the DC probably belongs to
 */
void vo_w32_release_dc(struct vo *vo, HWND wnd, HDC dc)
{
    struct vo_w32_state *w32 = vo->w32;
    if (w32->dev_hdc)
        return;
    ReleaseDC(wnd, dc);
}
