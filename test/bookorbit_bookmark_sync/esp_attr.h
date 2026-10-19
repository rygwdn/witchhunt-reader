#pragma once

// Host ownership tests have no RTC memory sections; retained state is ordinary static storage.
#define RTC_NOINIT_ATTR
