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

// DebugBridge and DebugCommand stubs
namespace Webserver {

void DebugCommand::WaitForCompletion([[maybe_unused]] const uint32_t timeout_ms) {}

DebugBridge& DebugBridge::Instance()
{
    static DebugBridge instance;
    return instance;
}

void DebugBridge::ExecuteCommand([[maybe_unused]] DebugCommand& cmd,
                                  [[maybe_unused]] const uint32_t timeout_ms) {}

void DebugBridge::ProcessRequests() {}

} // namespace Webserver
