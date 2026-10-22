// lf_hdr_display.h - is a screen in HDR, and how bright is its SDR white.
// Both come from the display configuration, where the screen is found by its
// GDI device name.
#pragma once
#include <windows.h>
#include <cstring>
#include <vector>

struct HdrDisplayState
{
    bool hdr_on = false;       // advanced colour on, and not forced wide-gamut SDR
    float sdr_white = 1.0f;    // SDR white in units of 80 nits (1.0 when unknown)
};

// LF_COLOR_SPACE (round 29, read once): how the output is handed to the
// screen - what a person picks under COLOURS / HDR.
//   0 auto     the HDR switch decides, as before
//   1 sRGB     8 bits, the usual SDR output - also on an HDR screen
//   2 linear   half float, linear light, SDR range (white at the screen's
//              SDR white): the compositor does the encoding
//   3 scRGB    half float; HDR kept when the screen is HDR, linear otherwise
static int ColorSpaceWanted()
{
    static const int mode = [] {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("LF_COLOR_SPACE", text, sizeof(text));
        const int value = (len > 0 && len < sizeof(text)) ? atoi(text) : 0;
        return value >= 0 && value <= 3 ? value : 0;
    }();
    return mode;
}

// LF_HDR=1: keep HDR through to the output (read once). The colour space
// picked says it instead when it is not "auto".
static bool HdrSwitchOn()
{
    static const bool on = [] {
        const int space = ColorSpaceWanted();
        if (space == 3) return true;
        if (space == 1 || space == 2) return false;
        char text[16] = {};
        GetEnvironmentVariableA("LF_HDR", text, sizeof(text));
        return strcmp(text, "1") == 0;
    }();
    return on;
}

// The active display paths, asked until the path list holds still (a mode
// change between the two calls makes the buffers too small).
static bool ActiveDisplayPaths(std::vector<DISPLAYCONFIG_PATH_INFO> &paths)
{
    for (int tries = 0; tries < 3; ++tries)
    {
        UINT32 path_count = 0, mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
            return false;
        paths.resize(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        const LONG answer = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                               modes.data(), nullptr);
        if (answer == ERROR_INSUFFICIENT_BUFFER) continue;
        if (answer != ERROR_SUCCESS) return false;
        paths.resize(path_count);
        return true;
    }
    return false;
}

static HdrDisplayState HdrDisplayOf(HMONITOR monitor)
{
    HdrDisplayState state;
    MONITORINFOEXW screen = {};
    screen.cbSize = sizeof(screen);
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!GetMonitorInfoW(monitor, &screen) || !ActiveDisplayPaths(paths)) return state;
    for (const DISPLAYCONFIG_PATH_INFO &path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId,
                         path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, screen.szDevice) != 0)
            continue;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colour = {};
        colour.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(colour), path.targetInfo.adapterId,
                         path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&colour.header) == ERROR_SUCCESS)
            state.hdr_on = colour.advancedColorEnabled && !colour.wideColorEnforced;
        DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
        level.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(level), path.targetInfo.adapterId,
                        path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel > 0)
            state.sdr_white = level.SDRWhiteLevel / 1000.0f;
        break;                  // the first path that shows this screen
    }
    return state;
}
