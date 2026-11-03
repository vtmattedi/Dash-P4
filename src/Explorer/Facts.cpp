#include "Facts.h"

#include <stdio.h>

#include <ArduinoJson.h>

DeviceFacts Facts_parse(const Device &device)
{
    DeviceFacts f;
    f.name = device.name;
    f.hardware = device.hardware;
    f.timezone = device.timezone;

    if (!device.infoJson.empty())
    {
        JsonDocument doc;
        if (!deserializeJson(doc, device.infoJson))
        {
            JsonObjectConst identity = doc["identity"];
            if (!identity.isNull())
            {
                f.id = identity["id"] | "";
                if (f.timezone.empty())
                {
                    f.timezone = identity["timezone"] | "";
                }
            }
            JsonObjectConst hw = doc["hardware"];
            if (!hw.isNull())
            {
                f.hasHardware = true;
                f.board = hw["board"] | "";
                f.chip = hw["chip"] | "";
                f.cores = hw["cores"] | 0;
                f.revision = hw["revision"] | 0;
                f.flashBytes = hw["flash_bytes"] | 0u;
                f.heapBytes = hw["heap_bytes"] | 0u;
                f.psramBytes = hw["psram_bytes"] | 0u;
            }
            JsonObjectConst build = doc["build"];
            if (!build.isNull() && build.size() > 0)
            {
                f.hasBuild = true;
                f.firmware = build["firmware_version"] | "";
                f.buildDate = build["date"] | "";
                f.buildTime = build["time"] | "";
                f.idfVersion = build["esp_idf_version"] | "";
            }
            JsonObjectConst boot = doc["boot"];
            if (!boot.isNull() && boot["reset_reason"].is<int>())
            {
                f.hasBoot = true;
                f.resetReason = boot["reset_reason"].as<int>();
            }
        }
    }

    if (!device.telemetrySystem.empty())
    {
        JsonDocument doc;
        if (!deserializeJson(doc, device.telemetrySystem))
        {
            f.hasSystem = true;
            f.uptimeMs = doc["uptime_ms"] | 0u;
            f.cpuMhz = doc["cpu_mhz"] | 0u;
            f.freeHeap = doc["free_heap_bytes"] | 0u;
            f.minFreeHeap = doc["min_free_heap_bytes"] | 0u;
            f.freePsram = doc["free_psram_bytes"] | 0u;
        }
    }

    if (!device.telemetryNetwork.empty())
    {
        JsonDocument doc;
        if (!deserializeJson(doc, device.telemetryNetwork))
        {
            f.hasNetwork = true;
            f.transportPreferred = doc["transport"]["preferred"] | "";
            f.transportActive = doc["transport"]["active"] | "";
            f.transportState = doc["transport"]["state"] | "";
            f.wifiState = doc["wifi_ip"]["state"] | "";
            f.ssid = doc["wifi_ip"]["ssid"] | "";
            f.ip = doc["wifi_ip"]["ip"] | "";
            f.rssi = doc["wifi_ip"]["rssi"] | 0;
            f.channel = doc["wifi_ip"]["channel"] | 0;
            f.mqttState = doc["mqtt"]["state"] | "";
            f.mqttProfile = doc["mqtt"]["profile"] | "";
            f.espNowState = doc["esp_now"]["state"] | "";
            f.gatewayConnected = doc["esp_now"]["gateway_connected"] | false;
            f.rttMs = doc["esp_now"]["rtt_ms"] | -1;
        }
    }
    return f;
}

std::string Facts_formatDuration(uint32_t ms)
{
    const uint32_t s = ms / 1000;
    char buf[40];
    if (s >= 86400)
    {
        snprintf(buf, sizeof(buf), "%ud %02uh %02um", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60));
    }
    else if (s >= 3600)
    {
        snprintf(buf, sizeof(buf), "%uh %02um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    }
    else if (s >= 60)
    {
        snprintf(buf, sizeof(buf), "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
    }
    else
    {
        snprintf(buf, sizeof(buf), "%us", (unsigned)s);
    }
    return buf;
}

std::string Facts_formatBytes(uint32_t bytes)
{
    char buf[32];
    if (bytes >= 1024 * 1024)
    {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    else
    {
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(bytes / 1024));
    }
    return buf;
}

const char *Facts_resetReason(int reason)
{
    switch (reason)
    {
    case 1:
        return "power-on";
    case 2:
        return "external pin";
    case 3:
        return "software restart";
    case 4:
        return "panic";
    case 5:
        return "interrupt watchdog";
    case 6:
        return "task watchdog";
    case 7:
        return "other watchdog";
    case 8:
        return "deep sleep wake";
    case 9:
        return "brownout";
    case 10:
        return "SDIO";
    default:
        return "unknown";
    }
}
