#pragma once

// Temporary headless-test instrumentation. Keep both consoles disabled in
// production builds. Set either value to 1 here for a normal Arduino IDE
// compile; no custom compiler flags are required.
#ifndef SESSION_STORAGE_TEST_CONSOLE
#define SESSION_STORAGE_TEST_CONSOLE 0
#endif

#ifndef SESSION_SYNC_TEST_CONSOLE
#define SESSION_SYNC_TEST_CONSOLE 0
#endif
