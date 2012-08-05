/*
 *  This code is from the book "Programming with POSIX Threads", by
 *  David R. Butenhof.
 */

/*
 * rwlock.c
 *
 * This file implements the "read-write lock" synchronization
 * construct.
 *
 * A read-write lock allows a thread to lock shared data either
 * for shared read access or exclusive write access.
 *
 * The rwl_init() and rwl_destroy() functions, respectively,
 * allow you to initialize/create and destroy/free the
 * read-write lock.
 *
 * The rwl_rdlock() function locks a read-write lock for
 * shared read access, and rwl_rdunlock() releases the
 * lock. rwl_tryrdlock() attempts to lock a read-write lock
 * for read access, and returns EBUSY instead of blocking.
 *
 * The rwl_wrlock() function locks a read-write lock for
 * exclusive write access, and rwl_wrunlock() releases the
 * lock. rwl_trywrlock() attempts to lock a read-write lock
 * for write access, and returns EBUSY instead of blocking.
 */
#include <pthread.h>
#include <errno.h>
#include "rwlock.h"

#ifndef PTHREAD_RWLOCK_INITIALIZER
// #ifndef HAVE_PTHREAD_RWLOCK

/*
 * Initialize a read-write lock
 */
int rwl_init (rwlock_t *rwl)
{
    int status;

    rwl->r_active = 0;
    rwl->r_wait = rwl->w_wait = 0;
    rwl->w_active = 0;
    status = pthread_mutex_init (&rwl->mutex, NULL);
    if (status != 0)
        return status;
    status = pthread_cond_init (&rwl->read, NULL);
    if (status != 0) {
        /* if unable to create read CV, destroy mutex */
        pthread_mutex_destroy (&rwl->mutex);
        return status;
    }
    status = pthread_cond_init (&rwl->write, NULL);
    if (status != 0) {
        /* if unable to create write CV, destroy read CV and mutex */
        pthread_cond_destroy (&rwl->read);
        pthread_mutex_destroy (&rwl->mutex);
        return status;
    }
    rwl->valid = RWLOCK_VALID;
    return 0;
}

/*
 * Destroy a read-write lock
 */
int rwl_destroy (rwlock_t *rwl)
{
    int status, status1, status2;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;

    /*
     * Check whether any threads own the lock; report "BUSY" if
     * so.
     */
    if (rwl->r_active > 0 || rwl->w_active) {
        pthread_mutex_unlock (&rwl->mutex);
        return EBUSY;
    }

    /*
     * Check whether any threads are known to be waiting; report
     * EBUSY if so.
     */
    if (rwl->r_wait != 0 || rwl->w_wait != 0) {
        pthread_mutex_unlock (&rwl->mutex);
        return EBUSY;
    }

    rwl->valid = 0;
    status = pthread_mutex_unlock (&rwl->mutex);
    if (status != 0)
        return status;
    status = pthread_mutex_destroy (&rwl->mutex);
    status1 = pthread_cond_destroy (&rwl->read);
    status2 = pthread_cond_destroy (&rwl->write);
    return (status == 0 ? status : (status1 == 0 ? status1 : status2));
}

/*
 * Handle cleanup when the read lock condition variable
 * wait is cancelled.
 *
 * Simply record that the thread is no longer waiting,
 * and unlock the mutex.
 */
static void rwl_readcleanup (void *arg)
{
    rwlock_t    *rwl = (rwlock_t *)arg;

    rwl->r_wait--;
    pthread_mutex_unlock (&rwl->mutex);
}

/*
 * Lock a read-write lock for read access.
 */
int rwl_rdlock (rwlock_t *rwl)
{
    int status;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    if (rwl->w_active) {
        rwl->r_wait++;
        pthread_cleanup_push (rwl_readcleanup, (void*)rwl);
        while (rwl->w_active) {
            status = pthread_cond_wait (&rwl->read, &rwl->mutex);
            if (status != 0)
                break;
        }
        pthread_cleanup_pop (0);
        rwl->r_wait--;
    }
    if (status == 0)
        rwl->r_active++;
    pthread_mutex_unlock (&rwl->mutex);
    return status;
}

/*
 * Attempt to lock a read-write lock for read access (don't
 * block if unavailable).
 */
int rwl_tryrdlock (rwlock_t *rwl)
{
    int status, status2;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    if (rwl->w_active)
        status = EBUSY;
    else
        rwl->r_active++;
    status2 = pthread_mutex_unlock (&rwl->mutex);
    return (status2 != 0 ? status2 : status);
}

/*
 * Unlock a read-write lock from read access.
 */
int rwl_rdunlock (rwlock_t *rwl)
{
    int status, status2;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    rwl->r_active--;
    if (rwl->r_active == 0 && rwl->w_wait > 0)
        status = pthread_cond_signal (&rwl->write);
    status2 = pthread_mutex_unlock (&rwl->mutex);
    return (status2 == 0 ? status : status2);
}

/*
 * Handle cleanup when the write lock condition variable
 * wait is cancelled.
 *
 * Simply record that the thread is no longer waiting,
 * and unlock the mutex.
 */
static void rwl_writecleanup (void *arg)
{
    rwlock_t *rwl = (rwlock_t *)arg;

    rwl->w_wait--;
    pthread_mutex_unlock (&rwl->mutex);
}

/*
 * Lock a read-write lock for write access.
 */
int rwl_wrlock (rwlock_t *rwl)
{
    int status;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    if (rwl->w_active || rwl->r_active > 0) {
        rwl->w_wait++;
        pthread_cleanup_push (rwl_writecleanup, (void*)rwl);
        while (rwl->w_active || rwl->r_active > 0) {
            status = pthread_cond_wait (&rwl->write, &rwl->mutex);
            if (status != 0)
                break;
        }
        pthread_cleanup_pop (0);
        rwl->w_wait--;
    }
    if (status == 0)
        rwl->w_active = 1;
    pthread_mutex_unlock (&rwl->mutex);
    return status;
}

/*
 * Attempt to lock a read-write lock for write access. Don't
 * block if unavailable.
 */
int rwl_trywrlock (rwlock_t *rwl)
{
    int status, status2;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    if (rwl->w_active || rwl->r_active > 0)
        status = EBUSY;
    else
        rwl->w_active = 1;
    status2 = pthread_mutex_unlock (&rwl->mutex);
    return (status != 0 ? status : status2);
}

/*
 * Unlock a read-write lock from write access.
 */
int rwl_wrunlock (rwlock_t *rwl)
{
    int status;

    if (rwl->valid != RWLOCK_VALID)
        return EINVAL;
    status = pthread_mutex_lock (&rwl->mutex);
    if (status != 0)
        return status;
    rwl->w_active = 0;
    if (rwl->r_wait > 0) {
        status = pthread_cond_broadcast (&rwl->read);
        if (status != 0) {
            pthread_mutex_unlock (&rwl->mutex);
            return status;
        }
    } else if (rwl->w_wait > 0) {
        status = pthread_cond_signal (&rwl->write);
        if (status != 0) {
            pthread_mutex_unlock (&rwl->mutex);
            return status;
        }
    }
    status = pthread_mutex_unlock (&rwl->mutex);
    return status;
}

#endif /* ifndef HAVE_POSIX_RWLOCK */
