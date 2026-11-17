#include "PcPower.h"

#include <atomic>
#include <string>

#include <NightMare.h>

#include "App/LedStrip.h"
#include "Link/LinkServer.h"
#include "UI/Rest.h"

namespace
{
ManagedSensor<bool> g_linked("pc_linked");
ManagedAction g_monitorsOff("pc_monitors_off");
ManagedAction g_sleep("pc_sleep");

// Set by PcPower_sleep() on whatever task asked (an action arrives on the network's), acted on by the LVGL task.
std::atomic<bool> g_blankPanel{false};

ActionResult onMonitorsOff(ManagedAction &, const std::string &)
{
    return PcPower_monitorsOff() ? ActionResult{true, "OK"} : ActionResult{false, "no PC is linked"};
}

ActionResult onSleep(ManagedAction &, const std::string &)
{
    return PcPower_sleep() ? ActionResult{true, "OK"} : ActionResult{false, "no PC is linked"};
}
} // namespace

void PcPower_begin()
{
    g_monitorsOff.onInvoke = onMonitorsOff;
    g_sleep.onInvoke = onSleep;
    gResourcesManager.bindResource(&g_linked);
    gResourcesManager.bindResource(&g_monitorsOff);
    gResourcesManager.bindResource(&g_sleep);
}

void PcPower_tick()
{
    static int published = -1; // -1 nothing yet, else the last value sent
    const int linked = LinkServer_connected() ? 1 : 0;
    if (linked != published)
    {
        published = linked;
        g_linked.setValue(linked == 1);
    }

    if (g_blankPanel.exchange(false))
    {
        LedStrip_sleep(true);
        Rest_screenOffThen([]() { LedStrip_sleep(false); });
    }
}

bool PcPower_monitorsOff()
{
    return LinkServer_sleepPcDisplay() != 0;
}

bool PcPower_sleep()
{
    if (LinkServer_sleepPc() == 0)
    {
        return false;
    }
    g_blankPanel = true;
    return true;
}
