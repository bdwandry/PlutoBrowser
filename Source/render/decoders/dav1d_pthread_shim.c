/*
 * PlutoBrowser — dav1d_pthread_shim.c
 * Single-threaded pthread stubs for the vendored dav1d 1.5.1 decoder.
 *
 * dav1d gates EVERY thread primitive (mutex/cond/create/join) on n_tc > 1
 * (lib.c); our wrapper always decodes with n_threads=1 (and
 * max_frame_delay=1), so the task system never initializes and these
 * functions are LINK-ONLY — they are never executed. The two exceptions
 * that DO execute: pthread_once (dav1d_open's init_internal) and the
 * pthread_attr_* trio (stack-size query) — those are implemented for real
 * against newlib's pthread_once_t layout ({is_initialized,
 * init_executed}, see toolchain sys/_pthreadtypes.h).
 *
 * DEVICE ONLY (Makefile ifeq device block): the simulator links the real
 * macOS pthreads. Same precedent as Source/html/qjs_pthread_stubs.c.
 */
#include <pthread.h>
#include <stddef.h>

int pluto_dav1d_pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (once_control && init_routine && once_control->is_initialized &&
        !once_control->init_executed)
    {
        once_control->init_executed = 1;
        init_routine();
    }
    return 0;
}

int pluto_dav1d_pthread_attr_init(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pluto_dav1d_pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pluto_dav1d_pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    (void)attr;
    (void)stacksize;
    return 0;
}

/* Everything below is link-only under n_threads=1 (never executed).
 * pthread_create returns an error so any future multi-threaded use
 * fails loudly at dav1d_open instead of corrupting state. */
int pluto_dav1d_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)m;
    (void)a;
    return 0;
}

int pluto_dav1d_pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

int pluto_dav1d_pthread_mutex_lock(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

int pluto_dav1d_pthread_mutex_unlock(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

int pluto_dav1d_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    (void)c;
    (void)a;
    return 0;
}

int pluto_dav1d_pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pluto_dav1d_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    (void)c;
    (void)m;
    return 0;
}

int pluto_dav1d_pthread_cond_signal(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pluto_dav1d_pthread_cond_broadcast(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pluto_dav1d_pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*fn)(void *), void *arg)
{
    (void)t;
    (void)a;
    (void)fn;
    (void)arg;
    return 1; /* no threads on device: loud failure, never silent */
}

int pluto_dav1d_pthread_join(pthread_t t, void **res)
{
    (void)t;
    (void)res;
    return 0;
}
