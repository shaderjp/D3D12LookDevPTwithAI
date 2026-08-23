#include "FpsCamera.h"

#include <Xinput.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    struct NormalizedStick
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    XMVECTOR CameraForward(float yaw, float pitch)
    {
        return XMVector3Normalize(XMVectorSet(
            sinf(yaw) * cosf(pitch),
            sinf(pitch),
            cosf(yaw) * cosf(pitch),
            0.0f));
    }

    float NormalizeAxis(SHORT rawValue, float calibratedCenter)
    {
        const float raw = static_cast<float>(rawValue);
        const float delta = raw - calibratedCenter;
        const float range = delta >= 0.0f
            ? 32767.0f - calibratedCenter
            : calibratedCenter + 32768.0f;
        return std::clamp(delta / (std::max)(range, 1.0f), -1.0f, 1.0f);
    }

    NormalizedStick NormalizeStick(
        SHORT rawX,
        SHORT rawY,
        SHORT deadzone,
        float calibratedCenterX,
        float calibratedCenterY)
    {
        const float x = NormalizeAxis(rawX, calibratedCenterX);
        const float y = NormalizeAxis(rawY, calibratedCenterY);
        const float length = std::sqrt(x * x + y * y);
        const float normalizedDeadzone = static_cast<float>(deadzone) / 32767.0f;
        if (length <= normalizedDeadzone)
        {
            return {};
        }

        const float magnitude = std::clamp(
            (length - normalizedDeadzone) / (1.0f - normalizedDeadzone),
            0.0f,
            1.0f);
        const float inverseLength = 1.0f / length;
        return { x * inverseLength * magnitude, y * inverseLength * magnitude };
    }

    float NormalizeTrigger(BYTE value)
    {
        if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        {
            return 0.0f;
        }
        return static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
            static_cast<float>(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    }
}

namespace Bistro
{
    void FpsCamera::Reset(const XMFLOAT3& position, float yawRadians, float pitchRadians, float rollRadians)
    {
        m_position = position;
        m_yaw = yawRadians;
        m_pitch = std::clamp(pitchRadians, -1.45f, 1.45f);
        m_roll = std::isfinite(rollRadians) ? rollRadians : 0.0f;
    }

    void FpsCamera::SetActive(bool active)
    {
        m_active = active;
        if (!active)
        {
            m_lookActive = false;
        }
    }

    void FpsCamera::SetKeyDown(UINT virtualKey, bool down)
    {
        if (virtualKey < m_keyDown.size())
        {
            m_keyDown[virtualKey] = down;
        }
    }

    void FpsCamera::OnPointerButton(
        bool rightButtonDown,
        float x,
        float y)
    {
        m_lookActive = m_active && rightButtonDown;
        m_lastPointerX = x;
        m_lastPointerY = y;
    }

    void FpsCamera::OnPointerMove(float x, float y)
    {
        if (!m_active || !m_lookActive)
        {
            return;
        }

        const float sensitivity = 0.004f;
        m_yaw += (x - m_lastPointerX) * sensitivity;
        m_pitch += (y - m_lastPointerY) * sensitivity;
        m_pitch = std::clamp(m_pitch, -1.45f, 1.45f);
        m_lastPointerX = x;
        m_lastPointerY = y;
    }

    void FpsCamera::Update(float deltaSeconds)
    {
        if (!m_active)
        {
            return;
        }

        XINPUT_STATE gamepadState{};
        const bool gamepadWasConnected = m_gamepadConnected;
        const UINT previousGamepadIndex = m_gamepadIndex;
        m_gamepadConnected = false;
        if (m_gamepadEnabled)
        {
            if (m_gamepadIndex < XUSER_MAX_COUNT && XInputGetState(m_gamepadIndex, &gamepadState) == ERROR_SUCCESS)
            {
                m_gamepadConnected = true;
            }
            else
            {
                m_gamepadIndex = XUSER_MAX_COUNT;
                m_gamepadReconnectDelaySeconds -= deltaSeconds;
                if (m_gamepadReconnectDelaySeconds <= 0.0f)
                {
                    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
                    {
                        if (XInputGetState(index, &gamepadState) == ERROR_SUCCESS)
                        {
                            m_gamepadIndex = index;
                            m_gamepadConnected = true;
                            break;
                        }
                    }
                    m_gamepadReconnectDelaySeconds = m_gamepadConnected ? 0.0f : 1.0f;
                }
            }
        }

        if (m_gamepadConnected && (!gamepadWasConnected || previousGamepadIndex != m_gamepadIndex))
        {
            // A newly connected controller should be usable immediately. XInput
            // devices normally report zero-centered sticks, and the official
            // deadzones handle ordinary resting noise. Neutral calibration is
            // opt-in because sampling while a stick is held would otherwise
            // register an active input position as the center.
            UseDefaultGamepadCenter();
        }

        if (m_gamepadConnected && !m_gamepadCalibrated)
        {
            constexpr int CalibrationMotionThreshold = 2048;
            const auto movedDuringCalibration = [&](SHORT current, SHORT previous)
            {
                return std::abs(static_cast<int>(current) - static_cast<int>(previous)) > CalibrationMotionThreshold;
            };
            if (m_gamepadCalibrationHasLastSample &&
                (movedDuringCalibration(gamepadState.Gamepad.sThumbLX, m_gamepadCalibrationLastLX) ||
                 movedDuringCalibration(gamepadState.Gamepad.sThumbLY, m_gamepadCalibrationLastLY) ||
                 movedDuringCalibration(gamepadState.Gamepad.sThumbRX, m_gamepadCalibrationLastRX) ||
                 movedDuringCalibration(gamepadState.Gamepad.sThumbRY, m_gamepadCalibrationLastRY)))
            {
                RecalibrateGamepad();
            }

            m_gamepadCalibrationLastLX = gamepadState.Gamepad.sThumbLX;
            m_gamepadCalibrationLastLY = gamepadState.Gamepad.sThumbLY;
            m_gamepadCalibrationLastRX = gamepadState.Gamepad.sThumbRX;
            m_gamepadCalibrationLastRY = gamepadState.Gamepad.sThumbRY;
            m_gamepadCalibrationHasLastSample = true;
            m_gamepadCalibrationSumLX += gamepadState.Gamepad.sThumbLX;
            m_gamepadCalibrationSumLY += gamepadState.Gamepad.sThumbLY;
            m_gamepadCalibrationSumRX += gamepadState.Gamepad.sThumbRX;
            m_gamepadCalibrationSumRY += gamepadState.Gamepad.sThumbRY;
            ++m_gamepadCalibrationSamples;
            m_gamepadCalibrationElapsedSeconds += std::clamp(deltaSeconds, 0.0f, 0.1f);

            if (m_gamepadCalibrationElapsedSeconds >= 0.35f && m_gamepadCalibrationSamples >= 8)
            {
                const double inverseSampleCount = 1.0 / static_cast<double>(m_gamepadCalibrationSamples);
                m_gamepadCenterLX = static_cast<float>(m_gamepadCalibrationSumLX * inverseSampleCount);
                m_gamepadCenterLY = static_cast<float>(m_gamepadCalibrationSumLY * inverseSampleCount);
                m_gamepadCenterRX = static_cast<float>(m_gamepadCalibrationSumRX * inverseSampleCount);
                m_gamepadCenterRY = static_cast<float>(m_gamepadCalibrationSumRY * inverseSampleCount);
                m_gamepadCalibrated = true;
            }
        }

        const bool gamepadReady = m_gamepadConnected && m_gamepadCalibrated;
        const bool gamepadFastMove = gamepadReady &&
            (gamepadState.Gamepad.wButtons & (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_SHOULDER)) != 0;
        float speed = (m_keyDown[VK_SHIFT] || gamepadFastMove)
            ? m_fastMoveSpeed
            : m_baseMoveSpeed;
        speed *= deltaSeconds;

        XMVECTOR position = XMLoadFloat3(&m_position);
        XMVECTOR forward = CameraForward(m_yaw, 0.0f);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        if (m_keyDown['W']) position += forward * speed;
        if (m_keyDown['S']) position -= forward * speed;
        if (m_keyDown['A']) position -= right * speed;
        if (m_keyDown['D']) position += right * speed;
        if (m_keyDown['E']) position += up * speed;
        if (m_keyDown['Q']) position -= up * speed;

        if (gamepadReady)
        {
            const NormalizedStick move = NormalizeStick(
                gamepadState.Gamepad.sThumbLX,
                gamepadState.Gamepad.sThumbLY,
                XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
                m_gamepadCenterLX,
                m_gamepadCenterLY);
            const NormalizedStick look = NormalizeStick(
                gamepadState.Gamepad.sThumbRX,
                gamepadState.Gamepad.sThumbRY,
                XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,
                m_gamepadCenterRX,
                m_gamepadCenterRY);
            const float vertical = NormalizeTrigger(gamepadState.Gamepad.bRightTrigger) -
                NormalizeTrigger(gamepadState.Gamepad.bLeftTrigger);

            position += right * (move.x * speed);
            position += forward * (move.y * speed);
            position += up * (vertical * speed);

            m_yaw += look.x * m_gamepadLookSpeed * deltaSeconds;
            const float pitchDirection = m_gamepadInvertY ? -1.0f : 1.0f;
            m_pitch += look.y * pitchDirection * m_gamepadLookSpeed * deltaSeconds;
            m_pitch = std::clamp(m_pitch, -1.45f, 1.45f);
        }

        XMStoreFloat3(&m_position, position);
    }

    void FpsCamera::SetMoveSpeeds(float baseSpeed, float fastSpeed)
    {
        m_baseMoveSpeed = (std::max)(0.1f, baseSpeed);
        m_fastMoveSpeed = (std::max)(m_baseMoveSpeed, fastSpeed);
    }

    void FpsCamera::SetGamepadEnabled(bool enabled)
    {
        m_gamepadEnabled = enabled;
        if (!enabled)
        {
            m_gamepadConnected = false;
        }
        else
        {
            m_gamepadReconnectDelaySeconds = 0.0f;
        }
    }

    void FpsCamera::SetGamepadLookSpeed(float radiansPerSecond)
    {
        m_gamepadLookSpeed = std::clamp(radiansPerSecond, 0.25f, 6.0f);
    }

    void FpsCamera::SetGamepadInvertY(bool invertY)
    {
        m_gamepadInvertY = invertY;
    }

    void FpsCamera::RecalibrateGamepad()
    {
        m_gamepadCalibrationElapsedSeconds = 0.0f;
        m_gamepadCalibrationSumLX = 0.0;
        m_gamepadCalibrationSumLY = 0.0;
        m_gamepadCalibrationSumRX = 0.0;
        m_gamepadCalibrationSumRY = 0.0;
        m_gamepadCalibrationSamples = 0;
        m_gamepadCalibrationHasLastSample = false;
        m_gamepadCalibrated = false;
    }

    void FpsCamera::UseDefaultGamepadCenter()
    {
        m_gamepadCalibrationElapsedSeconds = 0.0f;
        m_gamepadCalibrationSumLX = 0.0;
        m_gamepadCalibrationSumLY = 0.0;
        m_gamepadCalibrationSumRX = 0.0;
        m_gamepadCalibrationSumRY = 0.0;
        m_gamepadCalibrationSamples = 0;
        m_gamepadCalibrationHasLastSample = false;
        m_gamepadCenterLX = 0.0f;
        m_gamepadCenterLY = 0.0f;
        m_gamepadCenterRX = 0.0f;
        m_gamepadCenterRY = 0.0f;
        m_gamepadCalibrated = true;
    }

    float FpsCamera::GetBaseMoveSpeed() const
    {
        return m_baseMoveSpeed;
    }

    float FpsCamera::GetFastMoveSpeed() const
    {
        return m_fastMoveSpeed;
    }

    bool FpsCamera::IsGamepadEnabled() const
    {
        return m_gamepadEnabled;
    }

    bool FpsCamera::IsGamepadConnected() const
    {
        return m_gamepadConnected;
    }

    bool FpsCamera::IsGamepadCalibrating() const
    {
        return m_gamepadEnabled && m_gamepadConnected && !m_gamepadCalibrated;
    }

    UINT FpsCamera::GetGamepadIndex() const
    {
        return m_gamepadIndex;
    }

    float FpsCamera::GetGamepadLookSpeed() const
    {
        return m_gamepadLookSpeed;
    }

    bool FpsCamera::GetGamepadInvertY() const
    {
        return m_gamepadInvertY;
    }

    float FpsCamera::GetYawRadians() const
    {
        return m_yaw;
    }

    float FpsCamera::GetPitchRadians() const
    {
        return m_pitch;
    }

    float FpsCamera::GetRollRadians() const
    {
        return m_roll;
    }

    XMMATRIX FpsCamera::GetViewMatrix() const
    {
        XMVECTOR eye = XMLoadFloat3(&m_position);
        XMVECTOR forward = CameraForward(m_yaw, m_pitch);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1.0e-8f)
        {
            right = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        }
        XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
        const XMVECTOR rolledUp = up * std::cos(m_roll) + right * std::sin(m_roll);
        return XMMatrixLookAtLH(eye, eye + forward, rolledUp);
    }

    XMFLOAT3 FpsCamera::GetPosition() const
    {
        return m_position;
    }
}
