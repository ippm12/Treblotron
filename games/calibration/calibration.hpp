/**
 * calibration.hpp
 *
 * Camera calibration and data collection screen. Displays feeds
 * from connected cameras and allows saving frames for training
 * dart detection models.
 */

#ifndef CALIBRATION_HPP
#define CALIBRATION_HPP

#include "game_lib/game.hpp"
#include "game_lib/input_hints.hpp"
#include <string>

struct SDL_Texture;
struct SDL_Surface;

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

    private:
        void renderCameraSlots();
        void renderStatusMessage();

#ifndef NDEBUG
        void saveFrames();
#endif

        FontID m_titleFontId;
        FontID m_bodyFontId;
        InputHints m_inputHints;

        uint32_t     m_cameraCount;
        std::string  m_statusMessage;
        float        m_statusTimer;

        // Cached textures and the surface pointer that produced them.
        // We only recreate the texture when the surface pointer changes.
        SDL_Texture*  m_cameraTextures[3]    = {nullptr, nullptr, nullptr};
        SDL_Surface*  m_lastCameraSurface[3] = {nullptr, nullptr, nullptr};
        float         m_lastFrameW[3]        = {};
        float         m_lastFrameH[3]        = {};
};

#endif // CALIBRATION_HPP
