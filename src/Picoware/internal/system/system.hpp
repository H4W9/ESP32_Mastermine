#pragma once
#include <Arduino.h>
namespace Picoware
{
    class System
    {
    public:
        System()
        {
        }
        /// Reboot into the bootloader (no separate bootloader on ESP32 — restart).
        static void bootloaderMode() noexcept
        {
            ESP.restart();
        }

        /// Get the current free heap size.
        static int freeHeap() noexcept
        {
            return (int)ESP.getFreeHeap();
        }

        /// Get the currnet free PSRAM size.
        static int freeHeapPSRAM() noexcept
        {
            return (int)ESP.getFreePsram();
        }

        /// Get if the board has WiFi (true for the ESP32-C5 Pancake).
        static bool isPicoW() noexcept
        {
            return true;
        }

        /// Get the current time in milliseconds since the device started.
        static uint32_t millis() noexcept
        {
            return ::millis();
        }

        /// Reboot the device.
        static void reboot() noexcept
        {
            ESP.restart();
        }

        /// Sleep for a given number of milliseconds.
        static void sleep(uint32_t ms) noexcept
        {
            delay(ms);
        }

        /// Get the total heap size.
        static int totalHeap() noexcept
        {
            return (int)ESP.getHeapSize();
        }

        /// Get the total PSRAM size.
        static int totalHeapPSRAM() noexcept
        {
            return (int)ESP.getPsramSize();
        }

        /// Get the total used heap size.
        static int usedHeap() noexcept
        {
            return (int)(ESP.getHeapSize() - ESP.getFreeHeap());
        }

        /// Get the total used PSRAM size.
        static int usedHeapPSRAM() noexcept
        {
            return (int)(ESP.getPsramSize() - ESP.getFreePsram());
        }
    };
} // namespace Picoware