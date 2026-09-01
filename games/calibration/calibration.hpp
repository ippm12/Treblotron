/**
 * calibration.hpp
 *
 * Camera calibration screen. Two modes:
 *   - Overview: 3-slot grid, navigate with arrows / D-pad / mouse, enter a slot
 *     to start wire calibration.
 *   - Calibrate: live view of one camera alongside a board diagram naming and
 *     pointing at the single intersection to click next — 20 outer-triple then
 *     20 outer-double, clockwise from the 20/1 wire.
 */

#ifndef CALIBRATION_HPP
#define CALIBRATION_HPP

#include "game_lib/game.hpp"
#include "game_lib/components/render_object.hpp"
#include "game_lib/input_hints.hpp"
#include "detect/wire_calibration.hpp"
#include "vision/vision.hpp"
#include <string>

struct SDL_Texture;

class CalibrationScreen : public Game
{
    public:
        CalibrationScreen();
        ~CalibrationScreen() override = default;

        Status init(FrameID frameId) override;
        void update(float deltaTime) override;
        void render() override;
        void shutdown() override;
        bool isPauseable() const override;

        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;
        void onMouseClick(float x, float y, uint8_t button) override;

    private:
        enum class Mode
        {
            Overview,
            Calibrate
        };

        struct Rect
        {
            float x = 0, y = 0, w = 0, h = 0;
            bool contains(float px, float py) const
            {
                return px >= x && px < x + w && py >= y && py < y + h;
            }
        };

        void renderOverview();
        void renderCalibrate();
        void renderSlot(uint32_t index, float slotX, float slotY);

        // Right-hand panel in Calibrate mode: says which intersection is next
        // in words, and shows it on a diagram of the board.
        void renderTargetGuide(uint32_t placed);
        void renderBoardDiagram(float cx, float cy, float radius,
                                uint32_t placed, const WireTarget& target);

        /** Draw one line of text; centred horizontally on x when centred. */
        void drawText(const std::string& text, FontID fontId, Color color,
                      float x, float y, uint32_t z, bool centred = false);
        /** Draw a filled dot of the given radius. */
        void drawDot(float x, float y, float radius, Color color, uint32_t z);
        void updateCameraTexture(uint32_t index);
        void showStatus(const std::string& msg);
        bool screenToFrame(uint32_t camIndex, const Rect& draw,
                           float sx, float sy, float& outX, float& outY) const;

#ifndef NDEBUG
        void saveFrames();
#endif

        FontID m_titleFontId;
        FontID m_bodyFontId;
        FontID m_guideFontId;
        FontID m_smallFontId;
        InputHints m_inputHints;

        Mode         m_mode = Mode::Overview;
        uint32_t     m_focusedSlot = 0;
        uint32_t     m_calibrateCam = 0;

        uint32_t     m_cameraCount;
        std::string  m_statusMessage;
        float        m_statusTimer;

        SDL_Texture*  m_cameraTextures[3]    = {nullptr, nullptr, nullptr};
        CameraFrame   m_cameraFrames[3];
        float         m_lastFrameW[3]        = {};
        float         m_lastFrameH[3]        = {};

        Rect          m_slotDrawRects[3];   // image-draw rects (not slot bg) for click mapping
        Rect          m_calibrateDrawRect;  // large view draw rect in Calibrate mode
};

#endif // CALIBRATION_HPP
