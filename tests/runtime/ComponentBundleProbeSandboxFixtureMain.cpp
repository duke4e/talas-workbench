#include "runtime/component_probe/macos/ComponentBundleProbeSandboxReport.h"

#include <xpc/xpc.h>

#include <atomic>
#include <cstdlib>

namespace
{
using namespace talas::runtime::component_probe;
constexpr char reportKey[] = "talas.component-probe.sandbox-report.v1";
std::atomic<bool> claimed{false};

void acceptPeer(xpc_connection_t peer)
{
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        if (event == nullptr || xpc_get_type(event) != XPC_TYPE_DICTIONARY
            || xpc_dictionary_get_count(event) != 0
            || claimed.exchange(true, std::memory_order_acq_rel))
        {
            xpc_connection_cancel(peer);
            return;
        }
        const auto report = runComponentBundleProbeSandboxReportV1();
        auto reply = xpc_dictionary_create_reply(event);
        if (reply == nullptr)
            std::_Exit(EXIT_FAILURE);
        xpc_dictionary_set_data(reply, reportKey, &report, sizeof(report));
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
        xpc_connection_send_barrier(peer, ^{
            xpc_connection_cancel(peer);
            std::_Exit(EXIT_SUCCESS);
        });
    });
    xpc_connection_resume(peer);
}
}

int main()
{
    xpc_main(acceptPeer);
}
