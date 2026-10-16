#pragma once

// Historical note: this shim was introduced during Device OS 3.3.x bring-up.
// Newer host libstdc++ still needs the platform gthread bridge available
// before user code pulls in <mutex> through application.h.
//
// The existing STM32 gthread adapter covers mutexes, but not the condition-
// variable entrypoints that newer libstdc++ expects during user-part builds.
// This header fills that gap for the user-part only, leaving the Device OS
// runtime implementation untouched.

#include <bits/gthr-default.h>
#include "concurrent_hal.h"

#ifndef __GTHREAD_COND_INIT_FUNCTION
#define __GTHREAD_COND_INIT_FUNCTION(cv) os_condition_variable_create(cv)
#endif

#ifndef __gthread_cond_destroy
static inline int __gthread_cond_destroy(__gthread_cond_t* cond) {
    os_condition_variable_destroy(*cond);
    return 0;
}
#endif

#ifndef __gthread_cond_wait
static inline int __gthread_cond_wait(__gthread_cond_t* cond, __gthread_mutex_t* mutex) {
    os_condition_variable_wait(*cond, mutex);
    return 0;
}
#endif

#ifndef __gthread_cond_signal
static inline int __gthread_cond_signal(__gthread_cond_t* cond) {
    os_condition_variable_notify_one(*cond);
    return 0;
}
#endif

#ifndef __gthread_cond_broadcast
static inline int __gthread_cond_broadcast(__gthread_cond_t* cond) {
    os_condition_variable_notify_all(*cond);
    return 0;
}
#endif
