// Entry point of the ESP32 firmware: starts the JS task, which runs the
// device (agent, debugger stub, installed package), as `qjsx agent` does on
// the desktop.
#include "board_esp32.h"
#include "platform.h"
#include "quickjs.h"
#include "runner.h"

#include <stdio.h>

#include "sdkconfig.h"

namespace {

/**
 * The JS task: becomes the device's JS thread, for good.
 */
void jsTask(void *) {
    // the agent's identity in HELLO; no token on a serial link
    agent::Identity identity = { "qjsx " CONFIG_IDF_TARGET, CONFIG_VERSION, nullptr, JS_GetBytecodeVersion() };
    agent::runDevice(identity);

    // runDevice returns only if the links could not start
    printf("qjsx: the device could not start\n");
}

} // namespace

/**
 * Called by ESP-IDF once the system is up: starts the JS task with its large
 * stack, on core 1, then returns (the main task ends).
 */
extern "C" void app_main(void) {
    platform::ThreadOptions options = { "js", board::JS_TASK_STACK, board::JS_TASK_PRIORITY, board::JS_TASK_CORE };
    if (!platform::startThread(&jsTask, nullptr, options)) {
        printf("qjsx: cannot start the JS task\n");
    }
}
