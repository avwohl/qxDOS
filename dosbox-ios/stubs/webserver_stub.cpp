/*
 * webserver_stub.cpp - Stub implementations for DOSBox web server
 *
 * The webserver/debug bridge is not needed on iOS.
 */

#include "webserver/bridge.h"
#include "config/config.h"

void WEBSERVER_Init() {}
void WEBSERVER_Destroy() {}
void WEBSERVER_AddConfigSection([[maybe_unused]] const ConfigPtr& conf) {}

// Added upstream in v0.83.0 and called from dosbox.cpp's main loop before it
// dispatches to the bridge, so it has to exist here or the link fails.  Saying
// "not enabled" also makes the Bridge::Instance() call in normal_loop()
// unreachable, which is the point of stubbing this out at all.
bool WEBSERVER_IsEnabled() { return false; }

// Webserver::DebugCommand and Webserver::DebugBridge were renamed to Command
// and Bridge upstream during the v0.83.0 bump.
namespace Webserver {

void Command::WaitForCompletion([[maybe_unused]] const uint32_t timeout_ms) {}

Bridge& Bridge::Instance()
{
    static Bridge instance;
    return instance;
}

void Bridge::ExecuteCommand([[maybe_unused]] Command& cmd,
                            [[maybe_unused]] const uint32_t timeout_ms) {}

void Bridge::ProcessRequests() {}

} // namespace Webserver
