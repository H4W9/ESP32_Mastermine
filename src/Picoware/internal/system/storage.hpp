#pragma once
#include <SD.h>
#include <ArduinoJson.h>
namespace Picoware
{
    // Storage is backed by the SD card, which the sketch setup() mounts on the
    // shared FSPI bus before any view runs. All paths are relative to the SD root.
    class Storage
    {
    public:
        Storage()
        {
        }
        /// SD is already mounted in setup(); nothing to do here.
        bool begin() noexcept
        {
            return true;
        }

        /// Load JSON from a file into `doc`. Returns true on success.
        bool deserialize(JsonDocument &doc, const char *filename) const noexcept
        {
            File file = SD.open(filename, FILE_READ);
            if (!file)
                return false;
            auto err = deserializeJson(doc, file);
            file.close();
            return !err;
        }

        /// How much heap is still free?
        size_t freeHeap() const noexcept
        {
            return ESP.getFreeHeap();
        }

        /// Read the entire contents of a text file into a String.
        String read(const char *filename) const
        {
            String result;
            File file = SD.open(filename, FILE_READ);
            if (file)
            {
                result = file.readString();
                file.close();
            }
            return result;
        }

        /// Write a JSON document to a file. Returns true on success.
        bool serialize(const JsonDocument &doc, const char *filename) const noexcept
        {
            File file = SD.open(filename, FILE_WRITE);
            if (!file)
                return false;
            serializeJson(doc, file);
            file.close();
            return true;
        }

        /// Write raw C-string data to a file. Returns true on success.
        bool write(const char *filename, const char *data) const noexcept
        {
            File file = SD.open(filename, FILE_WRITE);
            if (!file)
                return false;
            file.print(data);
            file.close();
            return true;
        }
    };
}