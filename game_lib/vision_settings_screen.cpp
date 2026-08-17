/**
 * vision_settings_screen.cpp
 *
 * The vision settings overlay and the always-on link indicator.
 *
 * These are GameManager members but live in their own file: game_manager.cpp is
 * already long, and this is a self-contained feature — everything here concerns
 * how darts get detected and nothing else.
 *
 * Settings is an overlay rather than a screen of its own because it has to be
 * reachable mid-game. loadGame() tears down whatever is running, and the whole
 * point of being able to open this during a leg is to fix a dropped connection,
 * or nudge a threshold that is double-counting, without abandoning the leg.
 *
 * Which rows exist depends on the build, not on a flag in here: a simulated
 * source has no thresholds to turn, and a local-inference build has no server
 * to address. Asking vision/ rather than testing DARTMATIC_USE_* keeps this file
 * free of build-configuration ifdefs.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string>
#include <vector>

#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_manager_class.hpp"
#include "vision/vision.hpp"
#include "vision/vision_link.hpp"
#include "vision/vision_settings.hpp"


namespace
{
    /** Above the pause overlay's own layers so the keyboard is never buried. */
    constexpr uint32_t SETTINGS_OVERLAY_Z = 500;

    /** Above normal game content, below the settings overlay. */
    constexpr uint32_t LINK_INDICATOR_Z = 480;

    /** Every row this screen can show, in display order. */
    enum SettingsRow : uint8_t
    {
        ROW_ADDRESS = 0,
        ROW_CONFIRM_FRAMES,
        ROW_CONFIRM_HOLD,
        ROW_CLEAR_HOLD,
        ROW_HAND_ENTER,
        ROW_CAPTURE,
        ROW_DEFAULTS,
        ROW_CLOSE,
    };

    /**
     * The rows this build actually shows.
     *
     * Recomputed rather than cached because it is four branches on two compile-
     * time constants, and a cached copy is one more thing that can be stale
     * while the cursor indexes into it.
     */
    std::vector<SettingsRow> visibleRows()
    {
        std::vector<SettingsRow> rows;
        if(visionUsesRemoteServer()) rows.push_back(ROW_ADDRESS);
        if(visionHasDetector())
        {
            rows.push_back(ROW_CONFIRM_FRAMES);
            rows.push_back(ROW_CONFIRM_HOLD);
            rows.push_back(ROW_CLEAR_HOLD);
            rows.push_back(ROW_HAND_ENTER);
            rows.push_back(ROW_CAPTURE);
            rows.push_back(ROW_DEFAULTS);
        }
        rows.push_back(ROW_CLOSE);
        return rows;
    }

    /** The row under the cursor, or ROW_CLOSE if the cursor has gone stale. */
    SettingsRow rowAt(uint8_t cursor)
    {
        const std::vector<SettingsRow> rows = visibleRows();
        if(cursor >= rows.size()) return ROW_CLOSE;
        return rows[cursor];
    }

    /** Longest address we accept: "255.255.255.255:65535" plus room for a name. */
    constexpr size_t SETTINGS_ADDRESS_MAX = 64;

    // ----- Panel layout -------------------------------------------------
    //
    // Derived from the pieces rather than hand-tuned, so the panel actually
    // contains what it draws. The input hints are the tall element: their
    // icons are ICON_SIZE square and hang *below* the y they are given, which
    // is what pushed them off the bottom of the first version of this panel.
    constexpr float HINT_ICON_SIZE = 54.0f;   // matches input_hints.cpp ICON_SIZE
    constexpr float PANEL_PAD      = 26.0f;   // breathing room inside every edge

    constexpr float ROW_H         = 62.0f;
    constexpr float ROWS_TOP      = 214.0f;   // below the title and status block
    constexpr float DESC_LINE_H   = 44.0f;    // what the selected row does
    constexpr float STATUS_LINE_H = 46.0f;    // the transient "Saved" line

    /** Widest the panel ever gets; sized for the longest row plus its value. */
    constexpr float PANEL_W = 980.0f;

    /** Where a row's value starts, so numbers line up down the panel. */
    constexpr float VALUE_COL = 520.0f;

    /** Total rows this build shows — the panel is sized to fit exactly them. */
    float panelHeight(size_t rowCount)
    {
        return ROWS_TOP
             + static_cast<float>(rowCount) * ROW_H
             + DESC_LINE_H
             + STATUS_LINE_H
             + HINT_ICON_SIZE
             + PANEL_PAD * 2.0f;
    }

    Color linkColor(VisionLinkState state)
    {
        switch(state)
        {
            case VisionLinkState::Healthy:       return {70, 200, 100};
            case VisionLinkState::Degraded:      return {225, 185, 60};
            case VisionLinkState::Disconnected:  return {225, 80, 70};
            case VisionLinkState::NotApplicable: break;
        }
        return {120, 120, 130};
    }

    const char* linkLabel(VisionLinkState state)
    {
        switch(state)
        {
            case VisionLinkState::Healthy:       return "Server connected";
            case VisionLinkState::Degraded:      return "Server slow";
            case VisionLinkState::Disconnected:  return "No inference server";
            case VisionLinkState::NotApplicable: break;
        }
        return "";
    }

    const char* rowLabel(SettingsRow row)
    {
        switch(row)
        {
            case ROW_ADDRESS:        return "Server address";
            case ROW_CONFIRM_FRAMES: return "Detections to confirm";
            case ROW_CONFIRM_HOLD:   return "Hold before counting";
            case ROW_CLEAR_HOLD:     return "Board clear delay";
            case ROW_HAND_ENTER:     return "Hand detect delay";
            case ROW_CAPTURE:        return "Save every dart";
            case ROW_DEFAULTS:       return "Reset to defaults";
            case ROW_CLOSE:          return "Close";
        }
        return "";
    }

    /**
     * One line saying what the selected row does.
     *
     * These are not self-explanatory numbers, and the person adjusting them is
     * standing at a dartboard rather than reading the source. Without this the
     * screen is a row of magic constants.
     */
    const char* rowHelp(SettingsRow row)
    {
        switch(row)
        {
            case ROW_ADDRESS:
                return "Where the inference server is - host or host:port.";
            case ROW_CONFIRM_FRAMES:
                return "How many separate looks must agree before a dart is scored.";
            case ROW_CONFIRM_HOLD:
                return "Raise this if a single throw is being counted twice.";
            case ROW_CLEAR_HOLD:
                return "Quiet time before a turn can end. Raise if turns end early.";
            case ROW_HAND_ENTER:
                return "How long a hand must be visible before collection starts.";
            case ROW_CAPTURE:
                return "Writes the frames behind every dart, for retraining.";
            case ROW_DEFAULTS:
                return "Put every setting above back to the shipped value.";
            case ROW_CLOSE:
                return "";
        }
        return "";
    }

    /** Adjustment step for a numeric row; 0 for rows that aren't numbers. */
    int rowStep(SettingsRow row)
    {
        switch(row)
        {
            case ROW_CONFIRM_FRAMES: return 1;
            case ROW_CONFIRM_HOLD:   return 50;
            case ROW_CLEAR_HOLD:     return 100;
            case ROW_HAND_ENTER:     return 50;
            default:                 return 0;
        }
    }

    /** Whether left/right does anything on this row. */
    bool rowIsAdjustable(SettingsRow row)
    {
        return rowStep(row) != 0 || row == ROW_CAPTURE;
    }

    /** Pointer to the field a numeric row edits, or nullptr. */
    int* rowField(SettingsRow row, DartVisionSettings& settings)
    {
        switch(row)
        {
            case ROW_CONFIRM_FRAMES: return &settings.tuning.confirmFrames;
            case ROW_CONFIRM_HOLD:   return &settings.tuning.confirmHoldMs;
            case ROW_CLEAR_HOLD:     return &settings.tuning.clearHoldMs;
            case ROW_HAND_ENTER:     return &settings.tuning.handEnterMs;
            default:                 return nullptr;
        }
    }
}


void GameManager::openSettings()
{
    m_settingsOpen    = true;
    m_settingsCursor  = 0;
    m_settingsEditing = false;
    m_settingsStatus.clear();
    m_settingsBuffer.clear();
    m_settingsKeyboard.close();
}


void GameManager::commitSettingsAddress()
{
    if(IS_STATUS_OK(setInferenceServerAddress(m_settingsBuffer)))
    {
        // No restart needed: the client re-reads the address on its next
        // reconnect, which is a couple of seconds away at most.
        m_settingsStatus = "Saved - reconnecting";
    }
    else
    {
        m_settingsStatus = "Could not save the address";
    }

    m_settingsEditing = false;
    m_settingsKeyboard.close();
    SDL_StopTextInput(SDL_GetKeyboardFocus());
}


/**
 * Apply an edited settings block and report the outcome on the status line.
 *
 * Saving on every nudge rather than on leaving the screen: an adjustment made
 * mid-leg is meant to take effect on the next throw, and there is no "apply"
 * step to forget. The write is five short lines to a local file.
 */
void GameManager::commitVisionSettings(const DartVisionSettings& settings)
{
    if(IS_STATUS_OK(setVisionSettings(settings)))
    {
        m_settingsStatus = "Saved";
    }
    else
    {
        // The value is live regardless — only the file failed — so say exactly
        // that rather than implying the change did not happen.
        m_settingsStatus = "Applied, but could not write ./config/vision.txt";
    }
}


void GameManager::handleSettingsText(const char* text)
{
    // Only reached on the physical-keyboard path; the on-screen keyboard keeps
    // its own buffer and is fed separately.
    if(!m_settingsEditing || m_settingsKeyboard.isOpen() || text == nullptr) return;

    const std::string input(text);
    if(m_settingsBuffer.length() + input.length() <= SETTINGS_ADDRESS_MAX)
    {
        m_settingsBuffer += input;
    }
}


void GameManager::handleSettingsKey(uint32_t keycode)
{
    // ----- Controller path: the on-screen keyboard owns the input -----
    if(m_settingsKeyboard.isOpen())
    {
        const VirtualKeyboardResult result = m_settingsKeyboard.handleKey(keycode);
        if(result == VirtualKeyboardResult::Confirmed)
        {
            m_settingsBuffer = m_settingsKeyboard.getText();
            commitSettingsAddress();
        }
        else if(result == VirtualKeyboardResult::Cancelled)
        {
            m_settingsEditing = false;
            m_settingsKeyboard.close();
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        }
        return;
    }

    // ----- Physical-keyboard path: type straight into the row -----
    if(m_settingsEditing)
    {
        switch(keycode)
        {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                commitSettingsAddress();
                break;

            case SDLK_ESCAPE:
                m_settingsEditing = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;

            case SDLK_BACKSPACE:
                if(!m_settingsBuffer.empty()) m_settingsBuffer.pop_back();
                break;

            default:
                break;
        }
        return;
    }

    // ----- Not editing: navigating and adjusting rows -----
    const std::vector<SettingsRow> rows = visibleRows();
    const uint8_t rowCount = static_cast<uint8_t>(rows.size());
    if(m_settingsCursor >= rowCount) m_settingsCursor = 0;
    const SettingsRow row = rows[m_settingsCursor];

    switch(keycode)
    {
        case SDLK_UP:
            m_settingsCursor = static_cast<uint8_t>((m_settingsCursor + rowCount - 1) % rowCount);
            m_settingsStatus.clear();
            break;

        case SDLK_DOWN:
            m_settingsCursor = static_cast<uint8_t>((m_settingsCursor + 1) % rowCount);
            m_settingsStatus.clear();
            break;

        case SDLK_LEFT:
        case SDLK_RIGHT:
        {
            const int direction = (keycode == SDLK_RIGHT) ? 1 : -1;

            DartVisionSettings settings = getVisionSettings();
            if(int* field = rowField(row, settings))
            {
                // setVisionSettings clamps, so walking off either end simply
                // parks at the limit rather than needing a bound check here.
                *field += direction * rowStep(row);
                commitVisionSettings(settings);
            }
            else if(row == ROW_CAPTURE)
            {
                settings.captureOnDetect = !settings.captureOnDetect;
                commitVisionSettings(settings);
            }
            break;
        }

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(row == ROW_ADDRESS)
            {
                m_settingsStatus.clear();
                m_settingsEditing = true;
                m_settingsBuffer  = getInferenceServerAddress();

                // Only put the on-screen keyboard up for someone who has no
                // real one to hand. Same rule the player rename uses.
                if(getLastInputDevice() == InputDevice::Gamepad)
                {
                    m_settingsKeyboard.open(m_settingsBuffer, SETTINGS_ADDRESS_MAX);
                }
                else
                {
                    SDL_StartTextInput(SDL_GetKeyboardFocus());
                }
            }
            else if(row == ROW_CAPTURE)
            {
                DartVisionSettings settings = getVisionSettings();
                settings.captureOnDetect = !settings.captureOnDetect;
                commitVisionSettings(settings);
            }
            else if(row == ROW_DEFAULTS)
            {
                commitVisionSettings(DartVisionSettings{});
                m_settingsStatus = "Reset to defaults";
            }
            else if(row == ROW_CLOSE)
            {
                m_settingsOpen = false;
            }
            break;

        case SDLK_ESCAPE:
            m_settingsOpen = false;
            break;

        default:
            break;
    }
}


void GameManager::renderSettings()
{
    auto add = [&](std::shared_ptr<RenderObject> obj) { renderQueueAdd(m_frameId, obj); };

    auto box = [&](float x, float y, float w, float h, Color color, uint32_t z)
    {
        auto b = std::make_shared<RenderShape>();
        b->m_type   = ShapeType::Box;
        b->m_color  = color;
        b->m_x      = x;
        b->m_y      = y;
        b->m_z      = z;
        b->m_width  = w;
        b->m_height = h;
        add(b);
    };

    const FontID rowFontId = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
    TTF_Font* rowFont = getFont(rowFontId);
    TTF_Font* barFont = getFont(m_barFontId);

    auto text = [&](const std::string& str, float x, float y, Color color, FontID font)
    {
        auto t = std::make_shared<RenderText>();
        t->m_text     = str;
        t->m_color    = color;
        t->m_fontId   = font;
        t->m_rotation = 0.0f;
        t->m_scaleX   = 1.0f;
        t->m_scaleY   = 1.0f;
        t->m_x        = x;
        t->m_y        = y;
        t->m_z        = SETTINGS_OVERLAY_Z + 3;
        add(t);
    };

    box(0.0f, 0.0f, 1920.0f, 1080.0f, {20, 20, 20}, SETTINGS_OVERLAY_Z);

    const std::vector<SettingsRow> rows = visibleRows();
    const DartVisionSettings settings = getVisionSettings();

    constexpr float panelW = PANEL_W;
    const float panelH = panelHeight(rows.size());
    const float panelX = (1920.0f - panelW) * 0.5f;
    const float panelY = (1080.0f - panelH) * 0.5f;
    box(panelX, panelY, panelW, panelH, {50, 50, 55}, SETTINGS_OVERLAY_Z + 1);

    const char* title = "Vision";
    int titleW = 0, titleH = 0;
    if(barFont) TTF_GetStringSize(barFont, title, 0, &titleW, &titleH);
    text(title, panelX + (panelW - titleW) * 0.5f, panelY + 26.0f, {200, 200, 200}, m_barFontId);

    // Live status, so the effect of an edit shows up without leaving the page.
    // A build that does its own inference has no link, and says what it is
    // running instead — otherwise the top of the panel is simply blank.
    const VisionLinkState state = getVisionLinkState();

    if(state != VisionLinkState::NotApplicable)
    {
        auto dot = std::make_shared<RenderShape>();
        dot->m_type  = ShapeType::Circle;
        dot->m_color = linkColor(state);
        dot->m_x     = panelX + 62.0f;
        dot->m_y     = panelY + 122.0f;
        dot->m_width = 26.0f;
        dot->m_z     = SETTINGS_OVERLAY_Z + 3;
        add(dot);

        text(linkLabel(state),      panelX + 96.0f, panelY + 104.0f, linkColor(state), rowFontId);
        text(getVisionLinkDetail(), panelX + 96.0f, panelY + 150.0f, {160, 160, 170},  rowFontId);
    }
    else
    {
        const std::string detection = getVisionDetectionStatus();
        text(visionHasDetector() ? "Detecting on this machine" : "Simulated darts - no cameras",
             panelX + 62.0f, panelY + 104.0f, {160, 160, 170}, rowFontId);
        if(!detection.empty())
        {
            text(detection, panelX + 62.0f, panelY + 150.0f, {130, 130, 140}, rowFontId);
        }
    }

    // Value column, per row. The address is the one that can be mid-edit, and
    // shows a caret while it is.
    auto valueText = [&](SettingsRow row) -> std::string
    {
        switch(row)
        {
            case ROW_ADDRESS:
            {
                if(m_settingsEditing) return m_settingsBuffer + "_";
                const std::string address = getInferenceServerAddress();
                return address.empty() ? std::string("(not set)") : address;
            }
            case ROW_CONFIRM_FRAMES: return std::to_string(settings.tuning.confirmFrames);
            case ROW_CONFIRM_HOLD:   return std::to_string(settings.tuning.confirmHoldMs) + " ms";
            case ROW_CLEAR_HOLD:     return std::to_string(settings.tuning.clearHoldMs)   + " ms";
            case ROW_HAND_ENTER:     return std::to_string(settings.tuning.handEnterMs)   + " ms";
            case ROW_CAPTURE:        return settings.captureOnDetect ? "On" : "Off";
            case ROW_DEFAULTS:
            case ROW_CLOSE:          break;
        }
        return {};
    };

    constexpr float rowH   = ROW_H;
    const float     rowW   = panelW - 96.0f;
    const float     rowX   = panelX + 48.0f;
    const float     startY = panelY + ROWS_TOP;

    for(size_t i = 0; i < rows.size(); i++)
    {
        const SettingsRow row      = rows[i];
        const float       rowY     = startY + static_cast<float>(i) * rowH;
        const bool        selected = (i == m_settingsCursor);

        if(selected)
        {
            box(rowX, rowY, rowW, rowH - 10.0f, {70, 70, 120}, SETTINGS_OVERLAY_Z + 2);
        }

        const Color labelColor = selected ? Color{255, 255, 255} : Color{170, 170, 180};

        int lw = 0, lh = 0;
        if(rowFont) TTF_GetStringSize(rowFont, rowLabel(row), 0, &lw, &lh);
        const float textY = rowY + (rowH - 10.0f - lh) * 0.5f;
        text(rowLabel(row), rowX + 20.0f, textY, labelColor, rowFontId);

        std::string value = valueText(row);
        if(value.empty()) continue;

        // Arrows only on the selected row: they are an instruction ("this one
        // moves"), not decoration, and on every row they would be noise.
        if(selected && rowIsAdjustable(row)) value = "< " + value + " >";
        text(value, rowX + VALUE_COL, textY,
             selected ? Color{235, 235, 255} : Color{150, 150, 160}, rowFontId);
    }

    const float hintsY  = panelY + panelH - PANEL_PAD - HINT_ICON_SIZE;
    const float statusY = hintsY - STATUS_LINE_H;
    const float descY   = statusY - DESC_LINE_H;

    const SettingsRow selectedRow = rowAt(m_settingsCursor);
    const char* help = rowHelp(selectedRow);
    if(help[0] != '\0')
    {
        int hw = 0, hh = 0;
        if(rowFont) TTF_GetStringSize(rowFont, help, 0, &hw, &hh);
        text(help, panelX + (panelW - hw) * 0.5f, descY, {140, 145, 160}, rowFontId);
    }

    if(!m_settingsStatus.empty())
    {
        int sw = 0, sh = 0;
        if(rowFont) TTF_GetStringSize(rowFont, m_settingsStatus.c_str(), 0, &sw, &sh);
        text(m_settingsStatus, panelX + (panelW - sw) * 0.5f, statusY, {150, 210, 160}, rowFontId);
    }

    // The keyboard covers the panel while typing, so the hints would just be
    // noise underneath it.
    if(m_settingsKeyboard.isOpen())
    {
        m_settingsKeyboard.render(m_frameId, SETTINGS_OVERLAY_Z + 10);
    }
    else
    {
        std::vector<InputHint> hints;
        if(m_settingsEditing)
        {
            hints = {{SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "save"},
                     {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,  "cancel"}};
        }
        else
        {
            if(rowIsAdjustable(selectedRow))
            {
                hints.push_back({SDLK_LEFT,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,  "change"});
            }
            hints.push_back({SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "select"});
            hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,  "close"});
        }
        m_inputHints.render(m_frameId, rowFontId, panelX + 48.0f, hintsY,
                            SETTINGS_OVERLAY_Z + 3, hints);
    }
}


void GameManager::renderLinkIndicator()
{
    const VisionLinkState state = getVisionLinkState();

    // Builds that do their own inference have no link to report on.
    if(state == VisionLinkState::NotApplicable) return;

    // The settings overlay already shows a larger version of the same thing.
    if(m_settingsOpen) return;

    // Always-on dot in the corner: green, amber or red at a glance.
    auto dot = std::make_shared<RenderShape>();
    dot->m_type  = ShapeType::Circle;
    dot->m_color = linkColor(state);
    dot->m_x     = 1880.0f;
    dot->m_y     = 34.0f;
    dot->m_width = 18.0f;
    dot->m_z     = LINK_INDICATOR_Z;
    renderQueueAdd(m_frameId, dot);

    if(state != VisionLinkState::Disconnected) return;

    // Down: say so in words and say what to press. Scoring has silently stopped
    // working, and without this the only symptom is darts not registering.
    const FontID fontId = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
    TTF_Font* font = getFont(fontId);
    const std::string message = std::string(linkLabel(state)) + " - press F1 for settings";

    int tw = 0, th = 0;
    if(font) TTF_GetStringSize(font, message.c_str(), 0, &tw, &th);

    const float bannerW = static_cast<float>(tw) + 56.0f;
    const float bannerH = static_cast<float>(th) + 22.0f;
    const float bannerX = (1920.0f - bannerW) * 0.5f;

    auto banner = std::make_shared<RenderShape>();
    banner->m_type   = ShapeType::Box;
    banner->m_color  = {70, 26, 24};
    banner->m_x      = bannerX;
    banner->m_y      = 18.0f;
    banner->m_z      = LINK_INDICATOR_Z;
    banner->m_width  = bannerW;
    banner->m_height = bannerH;
    renderQueueAdd(m_frameId, banner);

    auto label = std::make_shared<RenderText>();
    label->m_text     = message;
    label->m_color    = {245, 175, 170};
    label->m_fontId   = fontId;
    label->m_rotation = 0.0f;
    label->m_scaleX   = 1.0f;
    label->m_scaleY   = 1.0f;
    label->m_x        = bannerX + 28.0f;
    label->m_y        = 29.0f;
    label->m_z        = LINK_INDICATOR_Z + 1;
    renderQueueAdd(m_frameId, label);
}
