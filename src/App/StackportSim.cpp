#include "StackportSim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "Stackport.h"

namespace
{
bool g_active = false;
bool g_down = false, g_warn = false, g_run = false, g_offline = false;
int g_progress = 0;           // the fake deploy, 0..100
uint32_t g_runStepAt = 0;     // when the deploy last moved
uint32_t g_runDoneAt = 0;     // when it reached the end (0: not yet)
bool g_runGone = false;       // finished and removed
bool g_loop = false;
uint32_t g_goneAt = 0;
uint32_t g_wobbleAt = 0;
uint32_t g_phase = 0;

void send(const char *what, const std::string &json)
{
    const std::string topic = std::string("stackport/sim/") + what;
    NetMessage message{topic.c_str(), reinterpret_cast<const uint8_t *>(json.c_str()), json.size(), true, 0};
    Stackport_handle(message);
}

std::string project(int id, const char *name, const char *group, const char *state, int running, int total, int requests,
                    double p95)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"name\":\"%s\",\"group\":\"%s\",\"state\":\"%s\",\"containers\":{\"running\":%d,\"total\":%d},"
             "\"health\":\"%s\",\"traffic\":{\"windowS\":300,\"requests\":%d,\"serverErrors\":0,\"errorRatePct\":0.4,\"p95Ms\":%.0f}}",
             id, name, group, state, running, total, strcmp(state, "ready") == 0 ? "healthy" : "unhealthy", requests, p95);
    return buf;
}

void publishProjects()
{
    std::string json = "[";
    json += project(1, "api", "MW", "ready", 2, 2, 420, 180) + ",";
    json += project(2, "web", "MW", g_down ? "down" : "ready", g_down ? 0 : 1, 1, 310, 95) + ",";
    json += project(3, "db", "MW", g_down ? "problem" : "ready", 1, 1, 0, 0) + ",";
    json += project(4, "worker", "MW", "ready", 1, 1, 0, 0) + ",";
    json += project(5, "mail", "Services", "ready", 1, 1, 22, 240) + ",";
    json += project(6, "oauth", "Services", "ready", 1, 1, 60, 70) + ",";
    json += project(7, "notif", "Services", "ready", 1, 1, 45, 120) + ",";
    json += "{\"id\":8,\"name\":\"legacy\",\"group\":\"Old\",\"state\":\"paused\",\"containers\":{\"running\":0,\"total\":1}}]";
    send("state/projects", json);
}

void publishAlerts()
{
    std::string json = "[";
    auto add = [&](const char *id, const char *severity, const char *code, const char *subject, const char *message) {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s{\"id\":\"%s\",\"severity\":\"%s\",\"code\":\"%s\",\"scope\":\"project\",\"projectId\":0,"
                 "\"subject\":\"%s\",\"message\":\"%s\"}", json.size() > 1 ? "," : "", id, severity, code, subject, message);
        json += buf;
    };
    if (g_down)
    {
        add("project_down:2", "critical", "project_down", "web", "web is down: 0 of 1 containers running");
        add("project_unhealthy:3", "critical", "project_unhealthy", "db", "db runs but its health check fails");
    }
    if (g_warn || g_down)
    {
        add("project_latency:1", "warning", "project_latency", "api", "api p95 latency is 2.4 s over 5 minutes");
    }
    if (g_warn)
    {
        add("host_disk_low:0", "warning", "host_disk_low", "host", "Docker's disk is 91% full");
    }
    json += "]";
    send("state/alerts", json);
}

void publishActions()
{
    if (!g_run || g_runGone)
    {
        send("state/actions", "[]");
        return;
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
             "[{\"id\":\"sim-1\",\"kind\":\"deploy\",\"projectId\":1,\"projectName\":\"api\",\"startedAt\":\"x\",\"progress\":%d},"
             "{\"id\":\"sim-2\",\"kind\":\"restart\",\"projectId\":5,\"projectName\":\"mail\",\"startedAt\":\"x\",\"progress\":null}]",
             g_progress);
    send("state/actions", buf);
}

void publishHost(uint32_t nowMs)
{
    const double t = nowMs / 1000.0;
    char buf[300];
    snprintf(buf, sizeof(buf),
             "{\"cpuPercent\":%.1f,\"memPercent\":%.1f,\"memUsedMb\":5900,\"memTotalMb\":8000,\"load1\":%.2f,\"diskUsedPct\":%.0f,"
             "\"netRxBps\":%.0f,\"netTxBps\":%.0f,\"sampledAt\":\"x\"}",
             g_warn ? 93.0 : 30.0 + 25.0 * sin(t / 3.0), 72.0 + 3.0 * sin(t / 7.0), 0.5 + 0.4 * sin(t / 5.0), g_warn ? 91.0 : 82.0,
             350000.0 + 300000.0 * sin(t / 2.0) + 200000.0 * (g_phase & 1), 60000.0 + 55000.0 * sin(t / 4.0 + 1.0));
    send("state/host", buf);
}

void publishStatus()
{
    send("status", g_offline ? "{\"state\":\"offline\",\"reason\":\"socket_closed\",\"snapshotAt\":\"2026-11-12T12:00:00.000Z\"}"
                             : "{\"state\":\"online\",\"reason\":null,\"snapshotAt\":\"2026-11-12T12:00:00.000Z\"}");
}
} // namespace

bool StackportSim_start(const char *scenario, bool loop)
{
    const bool down = !strcmp(scenario, "down") || !strcmp(scenario, "all");
    const bool warn = !strcmp(scenario, "warn");
    const bool run = !strcmp(scenario, "run") || !strcmp(scenario, "all");
    const bool offline = !strcmp(scenario, "offline");
    if (!down && !warn && !run && !offline)
    {
        return false;
    }
    Stackport_reset(); // forgets the real instance too, so "sim" is the one followed from now on
    g_active = true;
    g_loop = loop;
    g_goneAt = 0;
    g_down = down;
    g_warn = warn;
    g_run = run;
    g_offline = offline;
    g_progress = 0;
    g_runGone = false;
    g_runDoneAt = 0;
    g_runStepAt = 0;
    g_wobbleAt = 0;
    g_phase = 0;
    publishStatus();
    publishProjects();
    publishAlerts();
    publishActions();
    publishHost(0);
    return true;
}

void StackportSim_stop()
{
    if (g_active)
    {
        g_active = false;
        Stackport_reset();
    }
}

bool StackportSim_active() { return g_active; }

void StackportSim_tick(uint32_t nowMs)
{
    if (!g_active)
    {
        return;
    }
    if (g_run && !g_runGone)
    {
        if (g_runStepAt == 0)
        {
            g_runStepAt = nowMs;
        }
        if (g_progress < 100 && nowMs - g_runStepAt >= 400)
        {
            g_runStepAt = nowMs;
            g_progress = g_progress + 4 > 100 ? 100 : g_progress + 4;
            if (g_progress == 100)
            {
                g_runDoneAt = nowMs;
            }
            publishActions();
        }
        else if (g_progress == 100 && nowMs - g_runDoneAt >= 1500)
        {
            g_runGone = true; // the run reached the end and is over
            g_goneAt = nowMs;
            publishActions();
        }
    }
    else if (g_run && g_loop && nowMs - g_goneAt >= 4000)
    {
        g_runGone = false; // and again
        g_progress = 0;
        g_runStepAt = nowMs;
        publishActions();
    }
    if (nowMs - g_wobbleAt >= 1000)
    {
        g_wobbleAt = nowMs;
        ++g_phase;
        publishHost(nowMs);
    }
}
