#pragma once

#if defined(__GNUC__) && (__GNUC__ >= 13)
#include <bits/gthr.h>

#ifndef __GTHREAD_COND_INIT
#define __GTHREAD_COND_INIT 0
#endif

#ifndef __GTHREAD_COND_INIT_FUNCTION
#define __GTHREAD_COND_INIT_FUNCTION(cond) do { *(cond) = 0; } while (0)
#endif

extern "C" {
int __gthread_cond_destroy(__gthread_cond_t*);
int __gthread_cond_wait(__gthread_cond_t*, __gthread_mutex_t*);
int __gthread_cond_signal(__gthread_cond_t*);
int __gthread_cond_broadcast(__gthread_cond_t*);
}

#endif
