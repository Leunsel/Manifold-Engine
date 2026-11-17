#include "UI/NopSled.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "App/Sounds.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace Manifold::UI
{
    namespace
    {
        constexpr float kWidth = 712.0f;
        constexpr float kHeight = 480.0f;
        constexpr float kLeft = 108.0f;
        constexpr float kCell = 37.0f;
        constexpr float kRight = kLeft + kCell * NopSledWindow::kColumns;
        constexpr float kCeiling = 34.0f;
        constexpr float kBrickTop = 40.0f;
        constexpr float kCellHeight = 19.0f;
        constexpr float kBrickBottom = kBrickTop + kCellHeight * NopSledWindow::kRows;
        constexpr float kPaddleY = 440.0f;
        constexpr float kPaddleHeight = 14.0f;
        constexpr float kPaddleNormal = 96.0f;
        constexpr float kPaddleWide = 146.0f;
        constexpr float kRadius = 5.0f;
        constexpr float kBaseSpeed = 300.0f;
        constexpr float kCapsuleSpeed = 115.0f;
        constexpr float kMaxBounce = 1.08f;
        constexpr int kMaxLives = 5;

        struct LevelDef
        {
            const char* Name;
            const char* Rows[NopSledWindow::kRows];
            const char* Pool;
            std::uint64_t Address;
        };

        constexpr const char* kCodePool = "48 89 5C 24 08 57 48 83 EC 20 8B 81 9C 01 00 00 2B C2 89 81 85 C0 7F 0C 48 8B CF 41 B8 10 00 00 00 48 8D 15 3A 7F 01 00 8B D8 0F B6 C1 48 83 C4 20 5B "
                                          "33 C0 F3 0F 10 05 44 89 4C 24 18 4C 8B C2 66 0F 6F 45 33 D2 49 8B F8 74 1A 8B 47 08 3B 46 10 7C 05 89 46 10 48 63 47 0C";

        const LevelDef kLevels[] = {
            { "Player::TakeDamage",
                { "................", "################", "###B########B###", "################", "#A#####J######A#", "################", ".......R........", "................", "................" },
                kCodePool, 0x7FF61A2B3C40ull },
            { "Inventory::RemoveItem",
                { "................", ".......##.......", "......####......", ".....##BB##.....", "....###JJ###....", "...##########...", "..###A####A###..", ".######LL######.", "################" },
                kCodePool, 0x7FF61A2C0410ull },
            { "AntiCheat::Verify",
                { "AAAAAAAAAAAAAAAA", "A##############A", "A#B##########B#A", "A######RR######A", "A###J######J###A", "A##############A", "AAAAAA#LL#AAAAAA", "................", "................" },
                kCodePool, 0x7FF61A3D7700ull },
            { "Timer::Tick",
                { "################", "................", "#B####J##J####B#", "................", "AAAAAAAAAAAAAAAA", "................", "##L##########L##", "................", "#######RR#######" },
                kCodePool, 0x7FF61A2E91A0ull },
            { "Game::Over",
                { "....########....", "...##########...", "..###B####B###..", "..##BBB##BBB##..", "..############..", "...####JJ####...", "....##A##A##....", "....##A##A##....", ".....L....R....." },
                "DE AD BE EF", 0x7FF61A400000ull },
        };
        constexpr int kLevelCount = static_cast<int>(std::size(kLevels));

        std::vector<std::uint8_t> ParsePool(const char* text)
        {
            std::vector<std::uint8_t> bytes;
            unsigned value = 0;
            int digits = 0;
            for (const char* at = text; *at; ++at)
            {
                const char c = *at;
                const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (digit < 0)
                    continue;
                value = value * 16 + static_cast<unsigned>(digit);
                if (++digits == 2)
                {
                    bytes.push_back(static_cast<std::uint8_t>(value));
                    value = 0;
                    digits = 0;
                }
            }
            if (bytes.empty())
                bytes.push_back(0x90);
            return bytes;
        }

        ImVec4 PowerColor(int power)
        {
            switch (power)
            {
            case 1: return ManifoldTheme::Breakpoint;
            case 2: return ManifoldTheme::FlowJump;
            case 3: return ManifoldTheme::CodeLabel;
            case 4: return ManifoldTheme::FlowReturn;
            default: return ManifoldTheme::Text;
            }
        }

        const char* PowerName(int power)
        {
            switch (power)
            {
            case 1: return "INT3";
            case 2: return "JMP";
            case 3: return "LOCK";
            case 4: return "RET";
            default: return "";
            }
        }

        ImU32 Color(ImVec4 color, float alpha = 1.0f)
        {
            return ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, color.w * alpha));
        }

        std::string HexAddress(std::uint64_t value)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "%012llX", static_cast<unsigned long long>(value));
            return text;
        }
    }

    NopSledWindow::NopSledWindow(Workspace& workspace) : m_workspace(workspace)
    {
        m_best = std::max(0, workspace.Core().Options().NopSledBest);
        m_paddleX = (kLeft + kRight) * 0.5f;
        LoadLevel(0);
        m_phase = Phase::Title;
    }

    void NopSledWindow::Show()
    {
        m_open = true;
        m_bringToFront = true;
        if (m_phase == Phase::Playing)
            m_phase = Phase::Paused;
    }

    bool NopSledWindow::Animating() const
    {
        if (!m_open || !m_focused)
            return false;
        return m_phase == Phase::Playing || m_phase == Phase::Ready || m_phase == Phase::Cleared || !m_particles.empty() || !m_floaters.empty();
    }

    float NopSledWindow::Random()
    {
        m_seed ^= m_seed << 13;
        m_seed ^= m_seed >> 17;
        m_seed ^= m_seed << 5;
        return static_cast<float>(m_seed & 0xFFFFFF) / static_cast<float>(0x1000000);
    }

    float NopSledWindow::Speed() const
    {
        const float level = 1.0f + 0.06f * static_cast<float>(m_level % kLevelCount);
        const float loop = 1.0f + 0.12f * static_cast<float>(m_loop);
        const float progress = 1.0f + std::min(0.35f, 0.006f * static_cast<float>(m_broken));
        const float slow = m_breakTime > 0.0f ? 0.55f : 1.0f;
        return kBaseSpeed * level * loop * progress * slow;
    }

    float NopSledWindow::PaddleWidth() const
    {
        return m_lockTime > 0.0f ? kPaddleWide : kPaddleNormal;
    }

    int NopSledWindow::Alive() const
    {
        return static_cast<int>(std::count_if(m_bricks.begin(), m_bricks.end(), [](const Brick& brick) { return brick.Hits > 0; }));
    }

    ImVec4 NopSledWindow::BrickColor(int row, const Brick& brick) const
    {
        if (brick.Drop != Power::None)
            return PowerColor(static_cast<int>(brick.Drop));
        if (brick.Armored)
            return ManifoldTheme::FlowCall;
        const ImVec4 rows[] = { ManifoldTheme::CodeRegister, ManifoldTheme::CodeKeyword, ManifoldTheme::CodeNumber, ManifoldTheme::CodeString, ManifoldTheme::AddressText, ManifoldTheme::CodeSection };
        return rows[static_cast<std::size_t>(row) % std::size(rows)];
    }

    void NopSledWindow::NewGame()
    {
        m_score = 0;
        m_lives = 3;
        m_level = 0;
        m_loop = 0;
        m_particles.clear();
        m_floaters.clear();
        LoadLevel(0);
        m_phase = Phase::Ready;
        m_phaseTime = 0.0f;
    }

    void NopSledWindow::LoadLevel(int level)
    {
        const LevelDef& def = kLevels[level % kLevelCount];
        const std::vector<std::uint8_t> pool = ParsePool(def.Pool);
        std::size_t next = static_cast<std::size_t>(level) * 7 % pool.size();
        for (int row = 0; row < kRows; ++row)
        {
            for (int column = 0; column < kColumns; ++column)
            {
                Brick& brick = m_bricks[static_cast<std::size_t>(row * kColumns + column)];
                brick = Brick{};
                switch (def.Rows[row][column])
                {
                case '#':
                    brick.Byte = pool[next++ % pool.size()];
                    brick.Hits = 1;
                    break;
                case 'A':
                    brick.Byte = 0xE8;
                    brick.Hits = 2;
                    brick.Armored = true;
                    break;
                case 'B':
                    brick.Byte = 0xCC;
                    brick.Hits = 1;
                    brick.Drop = Power::Breakpoint;
                    break;
                case 'J':
                    brick.Byte = 0xEB;
                    brick.Hits = 1;
                    brick.Drop = Power::Jump;
                    break;
                case 'L':
                    brick.Byte = 0xF0;
                    brick.Hits = 1;
                    brick.Drop = Power::Lock;
                    break;
                case 'R':
                    brick.Byte = 0xC3;
                    brick.Hits = 1;
                    brick.Drop = Power::Return;
                    break;
                default:
                    break;
                }
            }
        }
        m_crashAt = def.Address;
        m_capsules.clear();
        m_lockTime = 0.0f;
        m_breakTime = 0.0f;
        m_combo = 0;
        m_broken = 0;
        ServeBall();
    }

    void NopSledWindow::ServeBall()
    {
        m_balls.clear();
        Ball ball;
        ball.X = m_paddleX;
        ball.Y = kPaddleY - kRadius - 1.0f;
        ball.Stuck = true;
        m_balls.push_back(ball);
    }

    void NopSledWindow::Launch()
    {
        const float speed = Speed();
        for (Ball& ball : m_balls)
        {
            if (!ball.Stuck)
                continue;
            const float angle = (Random() * 2.0f - 1.0f) * 0.35f;
            ball.Vx = std::sin(angle) * speed;
            ball.Vy = -std::cos(angle) * speed;
            ball.Stuck = false;
        }
        m_phase = Phase::Playing;
        m_phaseTime = 0.0f;
    }

    void NopSledWindow::Burst(float x, float y, ImVec4 color, int count, float speed)
    {
        for (int i = 0; i < count && m_particles.size() < 600; ++i)
        {
            const float angle = Random() * 6.2831853f;
            const float velocity = speed * (0.35f + Random() * 0.8f);
            Particle particle;
            particle.X = x;
            particle.Y = y;
            particle.Vx = std::cos(angle) * velocity;
            particle.Vy = std::sin(angle) * velocity - speed * 0.3f;
            particle.MaxLife = 0.45f + Random() * 0.45f;
            particle.Life = particle.MaxLife;
            particle.Size = 1.5f + Random() * 2.0f;
            particle.Color = color;
            m_particles.push_back(particle);
        }
    }

    void NopSledWindow::Float(float x, float y, std::string text, ImVec4 color)
    {
        if (m_floaters.size() > 40)
            m_floaters.erase(m_floaters.begin());
        m_floaters.push_back({ x, y, 1.0f, std::move(text), color });
    }

    void NopSledWindow::RememberBest()
    {
        if (m_score <= m_best)
            return;
        m_best = m_score;
        Settings& settings = m_workspace.Core().Options();
        settings.NopSledBest = m_best;
    }

    void NopSledWindow::HitBrick(int index, float x, float y)
    {
        Brick& brick = m_bricks[static_cast<std::size_t>(index)];
        const int row = index / kColumns;
        const int column = index % kColumns;
        const ImVec4 color = BrickColor(row, brick);
        const float centerX = kLeft + (static_cast<float>(column) + 0.5f) * kCell;
        const float centerY = kBrickTop + (static_cast<float>(row) + 0.5f) * kCellHeight;
        --brick.Hits;
        brick.Flash = 1.0f;
        if (brick.Hits > 0)
        {
            m_score += 5;
            Burst(x, y, color, 5, 90.0f);
            return;
        }
        ++m_broken;
        ++m_combo;
        const int base = brick.Armored ? 25 : brick.Drop != Power::None ? 15 : 10;
        const int multiplier = 1 + m_combo / 4;
        const int points = base * multiplier;
        m_score += points;
        Float(centerX, centerY - 6.0f, multiplier > 1 ? "+" + std::to_string(points) + "  x" + std::to_string(multiplier) : "+" + std::to_string(points), color);
        Burst(centerX, centerY, color, 10, 150.0f);
        if (brick.Drop != Power::None)
            m_capsules.push_back({ centerX, centerY, brick.Drop });
        RememberBest();
    }

    void NopSledWindow::Catch(Power power, float x)
    {
        const ImVec4 color = PowerColor(static_cast<int>(power));
        switch (power)
        {
        case Power::Breakpoint:
            m_breakTime = 6.0f;
            Float(x, kPaddleY - 18.0f, "INT3  slow motion", color);
            break;
        case Power::Jump:
        {
            const Ball* source = nullptr;
            for (const Ball& ball : m_balls)
            {
                if (!ball.Stuck)
                    source = &ball;
            }
            if (source && m_balls.size() < 12)
            {
                const Ball copy = *source;
                for (const float turn : { -0.45f, 0.45f })
                {
                    Ball extra = copy;
                    extra.Vx = copy.Vx * std::cos(turn) - copy.Vy * std::sin(turn);
                    extra.Vy = copy.Vx * std::sin(turn) + copy.Vy * std::cos(turn);
                    extra.TrailCount = 0;
                    m_balls.push_back(extra);
                }
            }
            Float(x, kPaddleY - 18.0f, "JMP  three balls", color);
            break;
        }
        case Power::Lock:
            m_lockTime = 12.0f;
            Float(x, kPaddleY - 18.0f, "LOCK  wide sled", color);
            break;
        case Power::Return:
            m_lives = std::min(kMaxLives, m_lives + 1);
            Float(x, kPaddleY - 18.0f, "RET  +1 thread", color);
            break;
        case Power::None:
            break;
        }
        Burst(x, kPaddleY, color, 14, 120.0f);
        m_score += 20;
        RememberBest();
    }

    void NopSledWindow::BallLost()
    {
        --m_lives;
        m_shake = 0.4f;
        m_combo = 0;
        m_capsules.clear();
        m_lockTime = 0.0f;
        m_breakTime = 0.0f;
        Burst(m_paddleX, kHeight - 8.0f, ManifoldTheme::Error, 24, 200.0f);
        if (m_lives <= 0)
        {
            m_lives = 0;
            m_balls.clear();
            m_phase = Phase::Over;
            m_phaseTime = 0.0f;
            RememberBest();
            Settings& settings = m_workspace.Core().Options();
            if (settings.NopSledBest == m_best && m_best > 0)
                m_workspace.Report(settings.Save(Paths::SettingsFile()));
            Sounds::Play(Sounds::Cue::Error);
            return;
        }
        Float(m_paddleX, kPaddleY - 18.0f, "-1 thread", ManifoldTheme::Error);
        ServeBall();
        m_phase = Phase::Ready;
        m_phaseTime = 0.0f;
    }

    bool NopSledWindow::CollideBricks(Ball& ball)
    {
        if (ball.Y - kRadius > kBrickBottom || ball.Y + kRadius < kBrickTop)
            return false;
        const int firstColumn = std::clamp(static_cast<int>(std::floor((ball.X - kRadius - kLeft) / kCell)), 0, kColumns - 1);
        const int lastColumn = std::clamp(static_cast<int>(std::floor((ball.X + kRadius - kLeft) / kCell)), 0, kColumns - 1);
        const int firstRow = std::clamp(static_cast<int>(std::floor((ball.Y - kRadius - kBrickTop) / kCellHeight)), 0, kRows - 1);
        const int lastRow = std::clamp(static_cast<int>(std::floor((ball.Y + kRadius - kBrickTop) / kCellHeight)), 0, kRows - 1);
        int best = -1;
        float bestDistance = kRadius * kRadius;
        float hitX = 0.0f;
        float hitY = 0.0f;
        for (int row = firstRow; row <= lastRow; ++row)
        {
            for (int column = firstColumn; column <= lastColumn; ++column)
            {
                const int index = row * kColumns + column;
                if (m_bricks[static_cast<std::size_t>(index)].Hits <= 0)
                    continue;
                const float x0 = kLeft + static_cast<float>(column) * kCell;
                const float y0 = kBrickTop + static_cast<float>(row) * kCellHeight;
                const float closestX = std::clamp(ball.X, x0, x0 + kCell);
                const float closestY = std::clamp(ball.Y, y0, y0 + kCellHeight);
                const float distance = (ball.X - closestX) * (ball.X - closestX) + (ball.Y - closestY) * (ball.Y - closestY);
                if (distance <= bestDistance)
                {
                    best = index;
                    bestDistance = distance;
                    hitX = closestX;
                    hitY = closestY;
                }
            }
        }
        if (best < 0)
            return false;
        const float dx = ball.X - hitX;
        const float dy = ball.Y - hitY;
        if (dx == 0.0f && dy == 0.0f)
        {
            ball.Vy = -ball.Vy;
            ball.Y += ball.Vy > 0.0f ? kRadius : -kRadius;
        }
        else if (std::abs(dx) > std::abs(dy))
        {
            ball.Vx = dx > 0.0f ? std::abs(ball.Vx) : -std::abs(ball.Vx);
            ball.X = hitX + (dx > 0.0f ? kRadius : -kRadius);
        }
        else
        {
            ball.Vy = dy > 0.0f ? std::abs(ball.Vy) : -std::abs(ball.Vy);
            ball.Y = hitY + (dy > 0.0f ? kRadius : -kRadius);
        }
        HitBrick(best, hitX, hitY);
        return true;
    }

    void NopSledWindow::MoveBall(Ball& ball, float dt)
    {
        const float speed = Speed();
        const float length = std::sqrt(ball.Vx * ball.Vx + ball.Vy * ball.Vy);
        if (length > 0.0f)
        {
            ball.Vx = ball.Vx / length * speed;
            ball.Vy = ball.Vy / length * speed;
        }
        const float minimum = speed * 0.22f;
        if (std::abs(ball.Vy) < minimum)
        {
            ball.Vy = ball.Vy < 0.0f ? -minimum : minimum;
            const float side = std::sqrt(std::max(0.0f, speed * speed - minimum * minimum));
            ball.Vx = ball.Vx < 0.0f ? -side : side;
        }
        ball.X += ball.Vx * dt;
        ball.Y += ball.Vy * dt;
        if (ball.X - kRadius < kLeft)
        {
            ball.X = kLeft + kRadius;
            ball.Vx = std::abs(ball.Vx);
        }
        else if (ball.X + kRadius > kRight)
        {
            ball.X = kRight - kRadius;
            ball.Vx = -std::abs(ball.Vx);
        }
        if (ball.Y - kRadius < kCeiling)
        {
            ball.Y = kCeiling + kRadius;
            ball.Vy = std::abs(ball.Vy);
        }
        const float half = PaddleWidth() * 0.5f;
        if (ball.Vy > 0.0f && ball.Y + kRadius >= kPaddleY && ball.Y - kRadius <= kPaddleY + kPaddleHeight && ball.X >= m_paddleX - half - kRadius &&
            ball.X <= m_paddleX + half + kRadius)
        {
            const float hit = std::clamp((ball.X - m_paddleX) / (half + kRadius), -1.0f, 1.0f);
            const float angle = hit * kMaxBounce;
            ball.Vx = std::sin(angle) * speed;
            ball.Vy = -std::cos(angle) * speed;
            ball.Y = kPaddleY - kRadius;
            m_combo = 0;
            Burst(ball.X, kPaddleY, ManifoldTheme::Accent, 4, 70.0f);
        }
        CollideBricks(ball);
    }

    void NopSledWindow::Update(float dt)
    {
        m_time += dt;
        m_phaseTime += dt;
        m_shake = std::max(0.0f, m_shake - dt);
        for (Particle& particle : m_particles)
        {
            particle.Life -= dt;
            particle.Vy += 420.0f * dt;
            particle.X += particle.Vx * dt;
            particle.Y += particle.Vy * dt;
        }
        m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(), [](const Particle& particle) { return particle.Life <= 0.0f; }), m_particles.end());
        for (Floater& floater : m_floaters)
        {
            floater.Life -= dt * 1.1f;
            floater.Y -= 28.0f * dt;
        }
        m_floaters.erase(std::remove_if(m_floaters.begin(), m_floaters.end(), [](const Floater& floater) { return floater.Life <= 0.0f; }), m_floaters.end());
        for (Brick& brick : m_bricks)
            brick.Flash = std::max(0.0f, brick.Flash - dt * 4.0f);

        if (m_phase == Phase::Cleared)
        {
            if (m_phaseTime > 2.6f)
            {
                ++m_level;
                if (m_level % kLevelCount == 0)
                    ++m_loop;
                LoadLevel(m_level);
                m_phase = Phase::Ready;
                m_phaseTime = 0.0f;
            }
            return;
        }
        if (m_phase != Phase::Playing && m_phase != Phase::Ready)
            return;
        for (Ball& ball : m_balls)
        {
            if (ball.Stuck)
            {
                ball.X = m_paddleX;
                ball.Y = kPaddleY - kRadius - 1.0f;
            }
        }
        if (m_phase == Phase::Ready)
            return;

        m_lockTime = std::max(0.0f, m_lockTime - dt);
        m_breakTime = std::max(0.0f, m_breakTime - dt);
        const float speed = Speed();
        const int steps = std::max(1, static_cast<int>(std::ceil(speed * dt / 3.0f)));
        for (Ball& ball : m_balls)
        {
            if (ball.Stuck)
                continue;
            for (int step = 0; step < steps; ++step)
                MoveBall(ball, dt / static_cast<float>(steps));
            ball.TrailTimer += dt;
            if (ball.TrailTimer >= 1.0f / 90.0f)
            {
                ball.TrailTimer = 0.0f;
                for (int i = static_cast<int>(ball.Trail.size()) - 1; i > 0; --i)
                    ball.Trail[static_cast<std::size_t>(i)] = ball.Trail[static_cast<std::size_t>(i - 1)];
                ball.Trail[0] = ImVec2(ball.X, ball.Y);
                ball.TrailCount = std::min(ball.TrailCount + 1, static_cast<int>(ball.Trail.size()));
            }
        }
        m_balls.erase(std::remove_if(m_balls.begin(), m_balls.end(), [](const Ball& ball) { return ball.Y - kRadius > kHeight + 10.0f; }), m_balls.end());

        const float half = PaddleWidth() * 0.5f;
        for (std::size_t i = 0; i < m_capsules.size();)
        {
            Capsule& capsule = m_capsules[i];
            capsule.Y += kCapsuleSpeed * dt;
            const bool caught = capsule.Y + 7.0f >= kPaddleY && capsule.Y - 7.0f <= kPaddleY + kPaddleHeight && capsule.X + 22.0f >= m_paddleX - half && capsule.X - 22.0f <= m_paddleX + half;
            if (caught)
            {
                const Power kind = capsule.Kind;
                const float x = capsule.X;
                m_capsules.erase(m_capsules.begin() + static_cast<std::ptrdiff_t>(i));
                Catch(kind, x);
                continue;
            }
            if (capsule.Y > kHeight + 10.0f)
            {
                m_capsules.erase(m_capsules.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        if (Alive() == 0)
        {
            m_bonus = 250 + 100 * m_lives;
            m_score += m_bonus;
            RememberBest();
            m_phase = Phase::Cleared;
            m_phaseTime = 0.0f;
            m_balls.clear();
            m_capsules.clear();
            for (int i = 0; i < 4; ++i)
                Burst(kLeft + (kRight - kLeft) * (0.2f + 0.2f * static_cast<float>(i)), kBrickTop + 60.0f, ManifoldTheme::Accent, 30, 220.0f);
            Sounds::Play(Sounds::Cue::Success);
            return;
        }
        if (m_balls.empty())
            BallLost();
    }

    void NopSledWindow::Input(bool focused, bool hovered, bool clicked, float mouseX)
    {
        if (!focused)
        {
            if (m_phase == Phase::Playing)
                m_phase = Phase::Paused;
            m_lastMouseX = -1.0f;
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        const float dt = std::min(io.DeltaTime, 1.0f / 30.0f);
        const bool left = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_A);
        const bool right = ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D);
        const bool moving = m_phase == Phase::Playing || m_phase == Phase::Ready;
        if (moving && (left != right))
        {
            m_keySpeed = std::min(620.0f, m_keySpeed + 3200.0f * dt);
            m_paddleX += (right ? 1.0f : -1.0f) * m_keySpeed * dt;
        }
        else
        {
            m_keySpeed = 260.0f;
            if (moving && hovered && m_lastMouseX >= 0.0f && std::abs(mouseX - m_lastMouseX) > 0.01f)
                m_paddleX = mouseX;
        }
        m_lastMouseX = hovered ? mouseX : -1.0f;
        const float half = PaddleWidth() * 0.5f;
        m_paddleX = std::clamp(m_paddleX, kLeft + half, kRight - half);

        const bool go = clicked || ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool pause = ImGui::IsKeyPressed(ImGuiKey_P, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        switch (m_phase)
        {
        case Phase::Title:
            if (go)
                NewGame();
            break;
        case Phase::Ready:
            if (go)
                Launch();
            break;
        case Phase::Playing:
            if (pause)
                m_phase = Phase::Paused;
            break;
        case Phase::Paused:
            if (go || ImGui::IsKeyPressed(ImGuiKey_P, false))
                m_phase = Phase::Playing;
            break;
        case Phase::Over:
            if (go && m_phaseTime > 0.6f)
                NewGame();
            break;
        case Phase::Cleared:
            break;
        }
    }

    void NopSledWindow::Hud()
    {
        const LevelDef& def = kLevels[m_level % kLevelCount];
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::CodeLabel);
        PushMono();
        ImGui::TextUnformatted(def.Name);
        PopMono();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Px(10.0f));
        Muted("level " + std::to_string(m_level + 1) + (m_loop > 0 ? "  ·  loop " + std::to_string(m_loop + 1) : std::string()));
        if (m_breakTime > 0.0f)
        {
            ImGui::SameLine(0.0f, Px(14.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Breakpoint);
            ImGui::Text("INT3 %.1f s", m_breakTime);
            ImGui::PopStyleColor();
        }
        if (m_lockTime > 0.0f)
        {
            ImGui::SameLine(0.0f, Px(14.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::CodeLabel);
            ImGui::Text("LOCK %.1f s", m_lockTime);
            ImGui::PopStyleColor();
        }

        const std::string score = std::to_string(m_score);
        const std::string best = "best " + std::to_string(m_best);
        std::string threads;
        for (int i = 0; i < m_lives; ++i)
            threads += ICON_FA_MICROCHIP " ";
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        const float scoreWidth = ImGui::CalcTextSize(score.c_str()).x;
        ImGui::PopFont();
        const float spacing = Px(14.0f);
        const float total = scoreWidth + spacing + ImGui::CalcTextSize(best.c_str()).x + spacing + ImGui::CalcTextSize(threads.c_str()).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - total));
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::TextUnformatted(score.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0.0f, spacing);
        Muted(best);
        ImGui::SameLine(0.0f, spacing);
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        ImGui::TextUnformatted(threads.c_str());
        ImGui::PopStyleColor();
        ItemTooltip("Threads left. Catch a RET to get one back.");
    }

    void NopSledWindow::Render(ImDrawList* draw, ImVec2 origin, float scale)
    {
        const auto at = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
        ImFont* mono = Fonts::Mono();
        const float byteSize = 13.0f * scale;
        const auto text = [&](ImFont* font, float size, float x, float y, ImU32 color, const char* value, bool center) {
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, value);
            const ImVec2 pos = at(x, y);
            draw->AddText(font, size, ImVec2(std::round(center ? pos.x - extent.x * 0.5f : pos.x), std::round(pos.y - extent.y * 0.5f)), color, value);
        };

        const float pulse = 0.5f + 0.5f * std::sin(m_time * 7.0f);
        draw->AddRectFilled(at(0.0f, 0.0f), at(kWidth, kHeight), Color(ManifoldTheme::Card), 10.0f * scale);
        draw->AddRect(at(0.0f, 0.0f), at(kWidth, kHeight), m_breakTime > 0.0f ? Color(ManifoldTheme::Breakpoint, 0.35f + 0.45f * pulse) : Color(ManifoldTheme::CardBorder), 10.0f * scale, 0, m_breakTime > 0.0f ? 2.0f * scale : 1.0f);
        draw->AddLine(at(kLeft - 8.0f, kCeiling - 2.0f), at(kLeft - 8.0f, kHeight - 12.0f), Color(ManifoldTheme::RowSeparator), 1.0f);
        draw->AddLine(at(kLeft - 8.0f, kCeiling - 2.0f), at(kRight, kCeiling - 2.0f), Color(ManifoldTheme::RowSeparator), 1.0f);

        char label[8];
        for (int column = 0; column < kColumns; ++column)
        {
            std::snprintf(label, sizeof(label), "%02X", column);
            text(mono, 11.0f * scale, kLeft + (static_cast<float>(column) + 0.5f) * kCell, 20.0f, Color(ManifoldTheme::Muted, 0.75f), label, true);
        }
        text(mono, 11.0f * scale, 12.0f, 20.0f, Color(ManifoldTheme::Muted, 0.75f), "Address", false);
        for (int row = 0; row < kRows; ++row)
        {
            const std::string address = HexAddress(m_crashAt + static_cast<std::uint64_t>(row) * 16u);
            text(mono, 12.0f * scale, 12.0f, kBrickTop + (static_cast<float>(row) + 0.5f) * kCellHeight, Color(ManifoldTheme::AddressText, 0.8f), address.c_str(), false);
        }

        for (int row = 0; row < kRows; ++row)
        {
            for (int column = 0; column < kColumns; ++column)
            {
                const Brick& brick = m_bricks[static_cast<std::size_t>(row * kColumns + column)];
                const float x0 = kLeft + static_cast<float>(column) * kCell;
                const float y0 = kBrickTop + static_cast<float>(row) * kCellHeight;
                const ImVec2 min = at(x0 + 2.0f, y0 + 2.0f);
                const ImVec2 max = at(x0 + kCell - 2.0f, y0 + kCellHeight - 2.0f);
                const float centerX = x0 + kCell * 0.5f;
                const float centerY = y0 + kCellHeight * 0.5f;
                if (brick.Hits > 0)
                {
                    const ImVec4 color = BrickColor(row, brick);
                    const bool special = brick.Drop != Power::None;
                    draw->AddRectFilled(min, max, Color(color, special ? 0.20f + 0.10f * pulse : 0.15f), 3.0f * scale);
                    draw->AddRect(min, max, Color(color, special ? 0.75f : 0.5f), 3.0f * scale, 0, std::max(1.0f, scale));
                    if (brick.Armored && brick.Hits > 1)
                        draw->AddRect(ImVec2(min.x + 2.0f * scale, min.y + 2.0f * scale), ImVec2(max.x - 2.0f * scale, max.y - 2.0f * scale), Color(color, 0.35f), 2.0f * scale);
                    if (brick.Armored && brick.Hits == 1)
                    {
                        draw->AddLine(at(x0 + 6.0f, y0 + 4.0f), at(x0 + 15.0f, y0 + 11.0f), Color(color, 0.6f), std::max(1.0f, scale));
                        draw->AddLine(at(x0 + 15.0f, y0 + 11.0f), at(x0 + 12.0f, y0 + 16.0f), Color(color, 0.6f), std::max(1.0f, scale));
                    }
                    std::snprintf(label, sizeof(label), "%02X", brick.Byte);
                    text(mono, byteSize, centerX, centerY, Color(color, brick.Armored && brick.Hits == 1 ? 0.7f : 1.0f), label, true);
                }
                else
                {
                    text(mono, byteSize, centerX, centerY, Color(ManifoldTheme::Muted, 0.22f), "90", true);
                }
                if (brick.Flash > 0.0f)
                    draw->AddRectFilled(min, max, Color(ManifoldTheme::Text, 0.45f * brick.Flash), 3.0f * scale);
            }
        }

        for (float x = kLeft; x < kRight; x += 10.0f)
            draw->AddLine(at(x, kHeight - 6.0f), at(x + 5.0f, kHeight - 6.0f), Color(ManifoldTheme::Error, 0.22f), 1.0f);
        {
            const char* unmapped = "unmapped";
            const ImVec2 extent = mono->CalcTextSizeA(10.0f * scale, FLT_MAX, 0.0f, unmapped);
            draw->AddText(mono, 10.0f * scale, ImVec2(at(kRight, kHeight - 14.0f).x - extent.x, at(0.0f, kHeight - 14.0f).y - extent.y * 0.5f), Color(ManifoldTheme::Error, 0.4f), unmapped);
        }

        for (const Capsule& capsule : m_capsules)
        {
            const ImVec4 color = PowerColor(static_cast<int>(capsule.Kind));
            const ImVec2 min = at(capsule.X - 22.0f, capsule.Y - 7.0f);
            const ImVec2 max = at(capsule.X + 22.0f, capsule.Y + 7.0f);
            draw->AddRectFilled(min, max, Color(color, 0.25f), 7.0f * scale);
            draw->AddRect(min, max, Color(color, 0.9f), 7.0f * scale, 0, std::max(1.0f, scale));
            text(mono, 11.0f * scale, capsule.X, capsule.Y, Color(color), PowerName(static_cast<int>(capsule.Kind)), true);
        }

        const float half = PaddleWidth() * 0.5f;
        for (int glow = 3; glow >= 1; --glow)
        {
            const float grow = static_cast<float>(glow) * 3.0f;
            draw->AddRectFilled(at(m_paddleX - half - grow, kPaddleY - grow), at(m_paddleX + half + grow, kPaddleY + kPaddleHeight + grow), Color(ManifoldTheme::Accent, 0.05f), (7.0f + grow) * scale);
        }
        draw->AddRectFilled(at(m_paddleX - half, kPaddleY), at(m_paddleX + half, kPaddleY + kPaddleHeight), Color(ManifoldTheme::Accent, 0.22f), 7.0f * scale);
        draw->AddRect(at(m_paddleX - half, kPaddleY), at(m_paddleX + half, kPaddleY + kPaddleHeight), Color(ManifoldTheme::Accent, 0.9f), 7.0f * scale, 0, std::max(1.0f, 1.5f * scale));
        text(mono, 11.5f * scale, m_paddleX, kPaddleY + kPaddleHeight * 0.5f, Color(ManifoldTheme::Accent), m_lockTime > 0.0f ? "90 90 90 90 90 90" : "90 90 90 90", true);

        for (const Ball& ball : m_balls)
        {
            for (int i = ball.TrailCount - 1; i >= 0; --i)
            {
                const float fade = 1.0f - static_cast<float>(i + 1) / static_cast<float>(ball.Trail.size() + 1);
                const ImVec2 point = ball.Trail[static_cast<std::size_t>(i)];
                draw->AddCircleFilled(at(point.x, point.y), kRadius * scale * (0.4f + 0.5f * fade), Color(m_breakTime > 0.0f ? ManifoldTheme::Breakpoint : ManifoldTheme::Accent, 0.25f * fade), 12);
            }
            draw->AddCircleFilled(at(ball.X, ball.Y), kRadius * 2.6f * scale, Color(m_breakTime > 0.0f ? ManifoldTheme::Breakpoint : ManifoldTheme::Accent, 0.12f), 20);
            draw->AddCircleFilled(at(ball.X, ball.Y), kRadius * scale, Color(ManifoldTheme::Text), 16);
        }

        for (const Particle& particle : m_particles)
        {
            const float life = std::clamp(particle.Life / particle.MaxLife, 0.0f, 1.0f);
            const float size = particle.Size * (0.5f + 0.5f * life);
            draw->AddRectFilled(at(particle.X - size, particle.Y - size), at(particle.X + size, particle.Y + size), Color(particle.Color, life));
        }
        ImFont* bold = Fonts::Bold();
        for (const Floater& floater : m_floaters)
            text(bold, 12.0f * scale, floater.X, floater.Y, Color(floater.Color, std::clamp(floater.Life, 0.0f, 1.0f)), floater.Text.c_str(), true);
    }

    void NopSledWindow::Overlay(ImDrawList* draw, ImVec2 origin, float scale)
    {
        if (m_phase == Phase::Playing)
            return;
        const auto at = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
        ImFont* bold = Fonts::Bold();
        ImFont* mono = Fonts::Mono();
        ImFont* regular = ImGui::GetFont();
        const auto line = [&](ImFont* font, float size, float y, ImVec4 color, const std::string& value) {
            const ImVec2 extent = font->CalcTextSizeA(size * scale, FLT_MAX, 0.0f, value.c_str());
            const ImVec2 pos = at(kWidth * 0.5f, y);
            draw->AddText(font, size * scale, ImVec2(std::round(pos.x - extent.x * 0.5f), std::round(pos.y - extent.y * 0.5f)), ImGui::GetColorU32(color), value.c_str());
        };
        const LevelDef& def = kLevels[m_level % kLevelCount];
        if (m_phase == Phase::Ready)
        {
            const std::string prompt = m_loop == 0 && m_level == 0 && m_score == 0 ? "Space or a click starts RIP" : "Space or a click starts RIP in " + std::string(def.Name);
            line(regular, 14.0f, kBrickBottom + 120.0f, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.85f), prompt);
            line(regular, 12.0f, kBrickBottom + 142.0f, ManifoldTheme::Muted, "Mouse or the arrow keys move the sled  ·  P pauses");
            return;
        }
        draw->AddRectFilled(at(0.0f, 0.0f), at(kWidth, kHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Card, 0.72f)), 10.0f * scale);
        switch (m_phase)
        {
        case Phase::Title:
            line(bold, 46.0f, 150.0f, ManifoldTheme::Accent, "NOP SLED");
            line(mono, 13.0f, 188.0f, ManifoldTheme::Muted, "90 90 90 90 90 90 90 90 90 90 90 90");
            line(regular, 15.0f, 232.0f, ManifoldTheme::Text, "Patch every byte of the function into a NOP.");
            line(regular, 15.0f, 254.0f, ManifoldTheme::Text, "Keep RIP on the sled, or it runs into unmapped memory.");
            line(regular, 13.0f, 296.0f, ManifoldTheme::Muted, "CC slows time  ·  EB splits RIP  ·  F0 widens the sled  ·  C3 returns a thread  ·  E8 takes two hits");
            line(regular, 13.0f, 318.0f, ManifoldTheme::Muted, "Mouse or the arrow keys move  ·  Space launches  ·  P pauses");
            line(bold, 15.0f, 362.0f, ManifoldTheme::Accent, "Press Space or click to start");
            if (m_best > 0)
                line(regular, 13.0f, 388.0f, ManifoldTheme::Muted, "Best " + std::to_string(m_best));
            break;
        case Phase::Paused:
            line(bold, 30.0f, 200.0f, ManifoldTheme::Breakpoint, "Breakpoint hit");
            line(mono, 13.0f, 236.0f, ManifoldTheme::Muted, "RIP = " + HexAddress(m_crashAt + 0x3Bu) + "   paused");
            line(regular, 14.0f, 272.0f, ManifoldTheme::Text, "P, Space or a click continues");
            break;
        case Phase::Cleared:
            line(bold, 40.0f, 190.0f, ManifoldTheme::Accent, "PATCHED");
            line(regular, 15.0f, 232.0f, ManifoldTheme::Text, std::string(def.Name) + " is a NOP sled now.");
            line(bold, 15.0f, 262.0f, ManifoldTheme::CodeLabel, "+" + std::to_string(m_bonus) + " bonus");
            line(regular, 13.0f, 292.0f, ManifoldTheme::Muted, "Next: " + std::string(kLevels[(m_level + 1) % kLevelCount].Name));
            break;
        case Phase::Over:
        {
            line(bold, 28.0f, 170.0f, ManifoldTheme::Error, "Unhandled exception");
            line(mono, 14.0f, 206.0f, ManifoldTheme::Error, "0xC0000005  access violation");
            line(mono, 13.0f, 230.0f, ManifoldTheme::Muted, "RIP = " + HexAddress(m_crashAt + 0x1000u) + " (unmapped)");
            line(bold, 18.0f, 276.0f, ManifoldTheme::Text, "Score " + std::to_string(m_score));
            const bool record = m_score > 0 && m_score >= m_best;
            line(regular, 14.0f, 302.0f, record ? ManifoldTheme::Accent : ManifoldTheme::Muted, record ? std::string("New best!") : "Best " + std::to_string(m_best));
            if (m_phaseTime > 0.6f)
                line(regular, 14.0f, 344.0f, ManifoldTheme::Text, "Space or a click tries again");
            break;
        }
        default:
            break;
        }
    }

    void NopSledWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_GAMEPAD "  NOP Sled###nopsled", &m_open, ImVec2(780.0f, 620.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(440.0f, 360.0f)))
        {
            m_focused = false;
            EndToolWindow();
            return;
        }
        m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        Hud();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float scale = std::max(0.2f, std::min(available.x / kWidth, available.y / kHeight));
        const ImVec2 size(kWidth * scale, kHeight * scale);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 origin(std::round(cursor.x + (available.x - size.x) * 0.5f), std::round(cursor.y));
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##field", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const ImGuiIO& io = ImGui::GetIO();
        Input(m_focused, hovered, clicked, (io.MousePos.x - origin.x) / scale);
        Update(std::min(io.DeltaTime, 1.0f / 30.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 shaken = origin;
        if (m_shake > 0.0f)
        {
            const float amount = m_shake * 10.0f * scale;
            shaken.x += (Random() * 2.0f - 1.0f) * amount;
            shaken.y += (Random() * 2.0f - 1.0f) * amount;
        }
        draw->PushClipRect(ImVec2(origin.x - 12.0f, origin.y - 12.0f), ImVec2(origin.x + size.x + 12.0f, origin.y + size.y + 12.0f), true);
        Render(draw, shaken, scale);
        Overlay(draw, shaken, scale);
        draw->PopClipRect();
        EndToolWindow();
        if (!m_open)
        {
            m_focused = false;
            if (m_phase == Phase::Playing)
                m_phase = Phase::Paused;
            RememberBest();
        }
    }
}
