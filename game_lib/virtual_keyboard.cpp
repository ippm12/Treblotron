/**
 * virtual_keyboard.cpp
 *
 * On-screen virtual keyboard for controller/gamepad text input.
 */

#include "game_lib/virtual_keyboard.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_lib/palette.hpp"
#include "game_lib/components/render_image.hpp"
#include "frame/render_queue.hpp"
#include "frame/image.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cmath>
#include <algorithm>


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float WINDOW_W     = 1920.0f;

static constexpr float KEY_W        = 120.0f;    // Standard key width
static constexpr float KEY_H        = 83.0f;    // Key height
static constexpr float ICON_SIZE    = 36.0f;    // Shortcut icon size on action keys
static constexpr float ICON_PAD     = 6.0f;     // Padding from key edge
static constexpr float KEY_GAP      = 9.0f;     // Gap between keys
static constexpr float ROW_GAP      = 9.0f;     // Gap between rows

static constexpr float KB_BG_TOP    = 360.0f;   // Background overlay top edge
static constexpr float KB_TEXT_Y    = 390.0f;   // Text buffer display Y
static constexpr float KB_ORIGIN_Y  = 480.0f;   // Top of first key row

// Colors
static constexpr Color COL_BG          = Palette::BG;
static constexpr Color COL_KEY_NORMAL  = Palette::BG_RAISED;
static constexpr Color COL_KEY_SELECT  = Palette::BG_SELECT;
static constexpr Color COL_KEY_ACTION  = {50, 65, 55};
static constexpr Color COL_KEY_CONFIRM = {40, 100, 60};
static constexpr Color COL_KEY_SHIFT   = {80, 80, 60};
static constexpr Color COL_TEXT_NORMAL = Palette::TEXT_DIM;
static constexpr Color COL_TEXT_SELECT = Palette::TEXT;
static constexpr Color COL_TEXT_BUFFER = Palette::TEXT;
static constexpr Color COL_CONFIRM_TXT = Palette::CONFIRM;


// ============================================================================
// Init / open / close
// ============================================================================

static const char* KB_BASE = "assets/images/kenney_input-prompts_1.4.1/Keyboard & Mouse/Default/";
static const char* XB_BASE = "assets/images/kenney_input-prompts_1.4.1/Xbox Series/Default/";

static ImageID loadIcon(const char* basePath, const char* filename)
{
    std::string path = std::string(basePath) + filename;
    return loadImage(path.c_str());
}

void VirtualKeyboard::init(FontID keyFont, FontID textFont)
{
    m_keyFont  = keyFont;
    m_textFont = textFont;
    m_inputHints.init();

    // Load shortcut icons for action keys
    m_iconShiftGamepad   = loadIcon(XB_BASE, "xbox_button_color_y.png");
    m_iconShiftKeyboard  = loadIcon(KB_BASE, "keyboard_tab.png");
    m_iconBackGamepad    = loadIcon(XB_BASE, "xbox_button_color_x.png");
    m_iconBackKeyboard   = loadIcon(KB_BASE, "keyboard_backspace.png");
    m_iconClearGamepad   = loadIcon(XB_BASE, "xbox_lb.png");
    m_iconClearKeyboard  = loadIcon(KB_BASE, "keyboard_home.png");
}


void VirtualKeyboard::open(const std::string& initialText, size_t maxLen)
{
    m_buffer    = initialText;
    m_maxLen    = maxLen;
    m_shifted   = false;
    m_cursorRow = 1;   // Start on the Q row
    m_cursorCol = 0;
    m_open      = true;
    buildLayout();
}


void VirtualKeyboard::close()
{
    m_open = false;
}


bool VirtualKeyboard::isOpen() const
{
    return m_open;
}


const std::string& VirtualKeyboard::getText() const
{
    return m_buffer;
}


// ============================================================================
// Layout
// ============================================================================

void VirtualKeyboard::buildLayout()
{
    m_rows.clear();

    // Row 0: numbers
    {
        std::vector<KeyDef> row;
        for(int i = 0; i < 10; i++)
        {
            std::string ch = std::to_string((i + 1) % 10);  // 1 2 3 ... 9 0
            row.push_back({ch, ch, 1.0f, KeyDef::Char});
        }
        m_rows.push_back(std::move(row));
    }

    // Row 1: QWERTYUIOP
    {
        const char* keys = "QWERTYUIOP";
        std::vector<KeyDef> row;
        for(int i = 0; keys[i]; i++)
        {
            std::string upper(1, keys[i]);
            std::string lower(1, keys[i] + 32);
            row.push_back({
                m_shifted ? upper : lower,
                m_shifted ? upper : lower,
                1.0f, KeyDef::Char
            });
        }
        m_rows.push_back(std::move(row));
    }

    // Row 2: ASDFGHJKL
    {
        const char* keys = "ASDFGHJKL";
        std::vector<KeyDef> row;
        for(int i = 0; keys[i]; i++)
        {
            std::string upper(1, keys[i]);
            std::string lower(1, keys[i] + 32);
            row.push_back({
                m_shifted ? upper : lower,
                m_shifted ? upper : lower,
                1.0f, KeyDef::Char
            });
        }
        m_rows.push_back(std::move(row));
    }

    // Row 3: SHIFT Z X C V B N M BACK
    {
        std::vector<KeyDef> row;
        row.push_back({m_shifted ? "SHIFT" : "Shift", "", 1.5f, KeyDef::Shift});

        const char* keys = "ZXCVBNM";
        for(int i = 0; keys[i]; i++)
        {
            std::string upper(1, keys[i]);
            std::string lower(1, keys[i] + 32);
            row.push_back({
                m_shifted ? upper : lower,
                m_shifted ? upper : lower,
                1.0f, KeyDef::Char
            });
        }
        row.push_back({"Back", "", 1.5f, KeyDef::Backspace});
        m_rows.push_back(std::move(row));
    }

    // Row 4: CLEAR + SPACE + CONFIRM
    {
        std::vector<KeyDef> row;
        row.push_back({"Clear", "", 1.5f, KeyDef::Clear});
        row.push_back({"Space", " ", 5.0f, KeyDef::Char});
        row.push_back({"OK", "", 2.0f, KeyDef::Confirm});
        m_rows.push_back(std::move(row));
    }
}


// ============================================================================
// Navigation helpers
// ============================================================================

float VirtualKeyboard::keyCenterX(int row, int col) const
{
    if(row < 0 || row >= static_cast<int>(m_rows.size()))
        return 0.0f;

    const auto& r = m_rows[row];
    float x = 0.0f;
    for(int i = 0; i < col && i < static_cast<int>(r.size()); i++)
    {
        x += r[i].width * KEY_W + KEY_GAP;
    }
    // Add half the current key's width
    if(col < static_cast<int>(r.size()))
    {
        x += r[col].width * KEY_W * 0.5f;
    }
    return x;
}


int VirtualKeyboard::closestColInRow(int fromRow, int fromCol, int toRow) const
{
    if(toRow < 0 || toRow >= static_cast<int>(m_rows.size()))
        return 0;

    float currentCenter = keyCenterX(fromRow, fromCol);
    const auto& targetRow = m_rows[toRow];

    int bestCol = 0;
    float bestDist = 1e9f;

    for(int i = 0; i < static_cast<int>(targetRow.size()); i++)
    {
        float center = keyCenterX(toRow, i);
        float dist = std::fabs(center - currentCenter);
        if(dist < bestDist)
        {
            bestDist = dist;
            bestCol = i;
        }
    }
    return bestCol;
}


void VirtualKeyboard::clampCursor()
{
    if(m_cursorRow < 0)
        m_cursorRow = static_cast<int>(m_rows.size()) - 1;
    if(m_cursorRow >= static_cast<int>(m_rows.size()))
        m_cursorRow = 0;

    int maxCol = static_cast<int>(m_rows[m_cursorRow].size()) - 1;
    if(m_cursorCol < 0)
        m_cursorCol = maxCol;
    if(m_cursorCol > maxCol)
        m_cursorCol = 0;
}


// ============================================================================
// Input handling
// ============================================================================

VirtualKeyboardResult VirtualKeyboard::handleKey(uint32_t keycode)
{
    if(!m_open) return VirtualKeyboardResult::Active;

    switch(keycode)
    {
        case SDLK_LEFT:
            m_cursorCol--;
            clampCursor();
            break;

        case SDLK_RIGHT:
            m_cursorCol++;
            clampCursor();
            break;

        case SDLK_UP:
        {
            int prevRow = m_cursorRow;
            int prevCol = m_cursorCol;
            m_cursorRow--;
            if(m_cursorRow < 0)
                m_cursorRow = static_cast<int>(m_rows.size()) - 1;
            m_cursorCol = closestColInRow(prevRow, prevCol, m_cursorRow);
            break;
        }

        case SDLK_DOWN:
        {
            int prevRow = m_cursorRow;
            int prevCol = m_cursorCol;
            m_cursorRow++;
            if(m_cursorRow >= static_cast<int>(m_rows.size()))
                m_cursorRow = 0;
            m_cursorCol = closestColInRow(prevRow, prevCol, m_cursorRow);
            break;
        }

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return pressCurrentKey();

        case SDLK_ESCAPE:
            return VirtualKeyboardResult::Cancelled;

        case SDLK_BACKSPACE:
        case SDLK_DELETE:
            if(!m_buffer.empty())
                m_buffer.pop_back();
            break;

        case SDLK_TAB:
            // Shift toggle (mapped from gamepad Y button)
            m_shifted = !m_shifted;
            buildLayout();
            clampCursor();
            break;

        case SDLK_HOME:
            // Clear buffer (mapped from gamepad LB button)
            m_buffer.clear();
            break;

        default:
            break;
    }

    return VirtualKeyboardResult::Active;
}


VirtualKeyboardResult VirtualKeyboard::handleTextInput(const char* text)
{
    if(!m_open || text == nullptr) return VirtualKeyboardResult::Active;

    std::string input(text);
    if(m_buffer.length() + input.length() <= m_maxLen)
    {
        m_buffer += input;
    }
    return VirtualKeyboardResult::Active;
}


VirtualKeyboardResult VirtualKeyboard::pressCurrentKey()
{
    if(m_cursorRow < 0 || m_cursorRow >= static_cast<int>(m_rows.size()))
        return VirtualKeyboardResult::Active;

    const auto& row = m_rows[m_cursorRow];
    if(m_cursorCol < 0 || m_cursorCol >= static_cast<int>(row.size()))
        return VirtualKeyboardResult::Active;

    const KeyDef& key = row[m_cursorCol];

    switch(key.action)
    {
        case KeyDef::Char:
            if(m_buffer.length() + key.value.length() <= m_maxLen)
            {
                m_buffer += key.value;
            }
            // Auto-unshift after typing one character
            if(m_shifted)
            {
                m_shifted = false;
                buildLayout();
            }
            break;

        case KeyDef::Backspace:
            if(!m_buffer.empty())
                m_buffer.pop_back();
            break;

        case KeyDef::Confirm:
            if(!m_buffer.empty())
                return VirtualKeyboardResult::Confirmed;
            break;

        case KeyDef::Clear:
            m_buffer.clear();
            break;

        case KeyDef::Shift:
            m_shifted = !m_shifted;
            buildLayout();
            clampCursor();
            break;
    }

    return VirtualKeyboardResult::Active;
}


// ============================================================================
// Rendering
// ============================================================================

void VirtualKeyboard::render(FrameID frameId, uint32_t zBase)
{
    if(!m_open) return;

    // --- Compute keyboard origin (centered horizontally) ---
    // Find the widest row to determine total keyboard width
    float maxRowWidth = 0.0f;
    for(const auto& row : m_rows)
    {
        float w = 0.0f;
        for(size_t i = 0; i < row.size(); i++)
        {
            w += row[i].width * KEY_W;
            if(i + 1 < row.size()) w += KEY_GAP;
        }
        maxRowWidth = std::max(maxRowWidth, w);
    }
    float kbOriginX = (WINDOW_W - maxRowWidth) * 0.5f;

    // --- Background overlay ---
    {
        auto bg = std::make_shared<RenderShape>();
        bg->m_type   = ShapeType::Box;
        bg->m_color  = COL_BG;
        bg->m_x      = 0.0f;
        bg->m_y      = KB_BG_TOP;
        bg->m_z      = zBase;
        bg->m_width  = WINDOW_W;
        bg->m_height = 1080.0f - KB_BG_TOP;
        renderQueueAdd(frameId, bg);
    }

    // --- Text buffer display ---
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = m_buffer + "_";
        text->m_color    = COL_TEXT_BUFFER;
        text->m_fontId   = m_textFont;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = kbOriginX;
        text->m_y        = KB_TEXT_Y;
        text->m_z        = zBase + 2;
        renderQueueAdd(frameId, text);
    }

    // --- Key rows ---
    float rowY = KB_ORIGIN_Y;
    for(int r = 0; r < static_cast<int>(m_rows.size()); r++)
    {
        const auto& row = m_rows[r];

        // Center each row within the keyboard width
        float rowWidth = 0.0f;
        for(size_t i = 0; i < row.size(); i++)
        {
            rowWidth += row[i].width * KEY_W;
            if(i + 1 < row.size()) rowWidth += KEY_GAP;
        }
        float keyX = kbOriginX + (maxRowWidth - rowWidth) * 0.5f;

        for(int c = 0; c < static_cast<int>(row.size()); c++)
        {
            const KeyDef& key = row[c];
            float keyW = key.width * KEY_W;
            bool isSelected = (r == m_cursorRow && c == m_cursorCol);

            // Key background color
            Color bgColor = COL_KEY_NORMAL;
            if(key.action == KeyDef::Confirm)
                bgColor = COL_KEY_CONFIRM;
            else if(key.action == KeyDef::Shift)
                bgColor = m_shifted ? Color{120, 120, 60} : COL_KEY_SHIFT;
            else if(key.action == KeyDef::Backspace || key.action == KeyDef::Clear)
                bgColor = COL_KEY_ACTION;

            if(isSelected)
                bgColor = COL_KEY_SELECT;

            // Key background
            {
                auto shape = std::make_shared<RenderShape>();
                shape->m_type   = ShapeType::Box;
                shape->m_color  = bgColor;
                shape->m_x      = keyX;
                shape->m_y      = rowY;
                shape->m_z      = zBase + 1;
                shape->m_width  = keyW;
                shape->m_height = KEY_H;
                renderQueueAdd(frameId, shape);
            }

            // Key label (centered in key using actual font metrics)
            {
                Color labelColor = isSelected ? COL_TEXT_SELECT : COL_TEXT_NORMAL;
                if(key.action == KeyDef::Confirm && !isSelected)
                    labelColor = COL_CONFIRM_TXT;

                int textW = 0, textH = 0;
                TTF_Font* font = getFont(m_keyFont);
                if(font)
                {
                    TTF_GetStringSize(font, key.label.c_str(), 0, &textW, &textH);
                }

                auto label = std::make_shared<RenderText>();
                label->m_text     = key.label;
                label->m_color    = labelColor;
                label->m_fontId   = m_keyFont;
                label->m_rotation = 0.0f;
                label->m_scaleX   = 1.0f;
                label->m_scaleY   = 1.0f;
                label->m_x        = keyX + (keyW - textW) * 0.5f;
                label->m_y        = rowY + (KEY_H - textH) * 0.5f;
                label->m_z        = zBase + 2;
                renderQueueAdd(frameId, label);
            }

            // Shortcut icon on action keys (top-right corner)
            {
                ImageID iconId = INVALID_IMAGE_ID;
                bool useGamepad = (getLastInputDevice() == InputDevice::Gamepad);

                if(key.action == KeyDef::Shift)
                    iconId = useGamepad ? m_iconShiftGamepad : m_iconShiftKeyboard;
                else if(key.action == KeyDef::Backspace)
                    iconId = useGamepad ? m_iconBackGamepad : m_iconBackKeyboard;
                else if(key.action == KeyDef::Clear)
                    iconId = useGamepad ? m_iconClearGamepad : m_iconClearKeyboard;

                if(iconId != INVALID_IMAGE_ID)
                {
                    auto icon = std::make_shared<RenderImage>();
                    icon->m_imageId = iconId;
                    icon->m_width   = ICON_SIZE;
                    icon->m_height  = ICON_SIZE;
                    icon->m_x       = keyX + keyW - ICON_SIZE - ICON_PAD;
                    icon->m_y       = rowY + ICON_PAD;
                    icon->m_z       = zBase + 3;
                    renderQueueAdd(frameId, icon);
                }
            }

            keyX += keyW + KEY_GAP;
        }

        rowY += KEY_H + ROW_GAP;
    }
}
