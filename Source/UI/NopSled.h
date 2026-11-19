#pragma once

#include "UI/ToolWindowBase.h"
#include "UI/Workspace.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class NopSledWindow : public ToolWindowBase
    {
    public:
        static constexpr int kColumns = 16;
        static constexpr int kRows = 9;

        explicit NopSledWindow(Workspace& workspace);
        void Draw() override;
        void Show();
        bool Animating() const;

    private:
        enum class Phase
        {
            Title,
            Ready,
            Playing,
            Paused,
            Cleared,
            Over
        };

        enum class Power
        {
            None,
            Breakpoint,
            Jump,
            Lock,
            Return
        };

        struct Brick
        {
            std::uint8_t Byte = 0x90;
            int Hits = 0;
            bool Armored = false;
            Power Drop = Power::None;
            float Flash = 0.0f;
        };

        struct Ball
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Vx = 0.0f;
            float Vy = 0.0f;
            bool Stuck = true;
            std::array<ImVec2, 10> Trail{};
            int TrailCount = 0;
            float TrailTimer = 0.0f;
        };

        struct Capsule
        {
            float X = 0.0f;
            float Y = 0.0f;
            Power Kind = Power::None;
        };

        struct Particle
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Vx = 0.0f;
            float Vy = 0.0f;
            float Life = 0.0f;
            float MaxLife = 1.0f;
            float Size = 2.0f;
            ImVec4 Color;
        };

        struct Floater
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Life = 1.0f;
            std::string Text;
            ImVec4 Color;
        };

        void NewGame();
        void LoadLevel(int level);
        void ServeBall();
        void Launch();
        void Update(float dt);
        void MoveBall(Ball& ball, float dt);
        bool CollideBricks(Ball& ball);
        void HitBrick(int index, float x, float y);
        void Catch(Power power, float x);
        void BallLost();
        void Burst(float x, float y, ImVec4 color, int count, float speed);
        void Float(float x, float y, std::string text, ImVec4 color);
        void Input(bool focused, bool hovered, bool clicked, float mouseX);
        void Render(ImDrawList* draw, ImVec2 origin, float scale);
        void Overlay(ImDrawList* draw, ImVec2 origin, float scale);
        void Hud();
        void RememberBest();
        float Random();
        float Speed() const;
        float PaddleWidth() const;
        int Alive() const;
        ImVec4 BrickColor(int row, const Brick& brick) const;

        Workspace& m_workspace;
        bool m_focused = false;
        Phase m_phase = Phase::Title;
        float m_phaseTime = 0.0f;
        float m_time = 0.0f;
        int m_level = 0;
        int m_loop = 0;
        int m_score = 0;
        int m_best = 0;
        int m_lives = 3;
        int m_combo = 0;
        int m_broken = 0;
        int m_bonus = 0;
        std::array<Brick, static_cast<std::size_t>(kColumns * kRows)> m_bricks{};
        std::vector<Ball> m_balls;
        std::vector<Capsule> m_capsules;
        std::vector<Particle> m_particles;
        std::vector<Floater> m_floaters;
        float m_paddleX = 0.0f;
        float m_lockTime = 0.0f;
        float m_breakTime = 0.0f;
        float m_shake = 0.0f;
        float m_keySpeed = 0.0f;
        float m_lastMouseX = -1.0f;
        std::uint32_t m_seed = 0x9E3779B9u;
        std::uint64_t m_crashAt = 0;
    };
}
