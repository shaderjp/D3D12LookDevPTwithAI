#pragma once

#include <DirectXMath.h>
#include <Windows.h>
#include <array>

namespace Bistro
{
    class FpsCamera
    {
    public:
        void Reset(const DirectX::XMFLOAT3& position, float yawRadians, float pitchRadians, float rollRadians = 0.0f);
        void SetActive(bool active);
        void SetKeyDown(UINT virtualKey, bool down);
        void OnPointerButton(bool rightButtonDown, float x, float y);
        void OnPointerMove(float x, float y);
        void Update(float deltaSeconds);
        void SetMoveSpeeds(float baseSpeed, float fastSpeed);
        void SetGamepadEnabled(bool enabled);
        void SetGamepadLookSpeed(float radiansPerSecond);
        void SetGamepadInvertY(bool invertY);
        void RecalibrateGamepad();
        void UseDefaultGamepadCenter();
        float GetBaseMoveSpeed() const;
        float GetFastMoveSpeed() const;
        bool IsGamepadEnabled() const;
        bool IsGamepadConnected() const;
        bool IsGamepadCalibrating() const;
        UINT GetGamepadIndex() const;
        float GetGamepadLookSpeed() const;
        bool GetGamepadInvertY() const;
        float GetYawRadians() const;
        float GetPitchRadians() const;
        float GetRollRadians() const;

        DirectX::XMMATRIX GetViewMatrix() const;
        DirectX::XMFLOAT3 GetPosition() const;

    private:
        DirectX::XMFLOAT3 m_position = DirectX::XMFLOAT3(-16.32f, 4.66f, -10.41f);
        float m_yaw = DirectX::XMConvertToRadians(18.1f);
        float m_pitch = DirectX::XMConvertToRadians(2.8f);
        float m_roll = 0.0f;
        float m_baseMoveSpeed = 17.0f;
        float m_fastMoveSpeed = 58.2f;
        float m_gamepadLookSpeed = 2.5f;
        float m_gamepadReconnectDelaySeconds = 0.0f;
        float m_gamepadCalibrationElapsedSeconds = 0.0f;
        double m_gamepadCalibrationSumLX = 0.0;
        double m_gamepadCalibrationSumLY = 0.0;
        double m_gamepadCalibrationSumRX = 0.0;
        double m_gamepadCalibrationSumRY = 0.0;
        float m_gamepadCenterLX = 0.0f;
        float m_gamepadCenterLY = 0.0f;
        float m_gamepadCenterRX = 0.0f;
        float m_gamepadCenterRY = 0.0f;
        UINT m_gamepadCalibrationSamples = 0;
        UINT m_gamepadIndex = 4;
        SHORT m_gamepadCalibrationLastLX = 0;
        SHORT m_gamepadCalibrationLastLY = 0;
        SHORT m_gamepadCalibrationLastRX = 0;
        SHORT m_gamepadCalibrationLastRY = 0;
        bool m_gamepadEnabled = true;
        bool m_gamepadConnected = false;
        bool m_gamepadCalibrated = true;
        bool m_gamepadCalibrationHasLastSample = false;
        bool m_gamepadInvertY = false;
        bool m_active = true;
        bool m_lookActive = false;
        std::array<bool, 256> m_keyDown{};
        float m_lastPointerX = 0.0f;
        float m_lastPointerY = 0.0f;
    };
}
