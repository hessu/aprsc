#ifndef RWLOCK_H
#define RWLOCK_H

/*
 * rwlock.h
 *
 * This header file describes the "reader/writer lock" synchronization
 * construct. The type rwlock_t describes the full state of the lock
 * including the POSIX 1003.1c synchronization objects necessary.
 *
 * A reader/writer lock allows a thread to lock shared data either for shared
 * read access or exclusive write access.
 *
 * The rwl_init() and rwl_destroy() functions, respectively, allow you to
 * initialize/create and destroy/free the reader/writer lock.
 */

#include <pthread.h>

#ifdef HAVE_PTHREAD_RWLOCK

#define rwlock_t pthread_rwlock_t

#define RWL_INITIALIZER PTHREAD_RWLOCK_INITIALIZER

#define rwl_init(tag) pthread_rwlock_init(tag, NULL)
#define rwl_destroy pthread_rwlock_destroy
#define rwl_rdlock pthread_rwlock_rdlock
#define rwl_tryrdlock pthread_rwlock_tryrdlock
#define rwl_rdunlock pthread_rwlock_unlock
#define rwl_wrlock pthread_rwlock_wrlock
#define rwl_trywrlock pthread_rwlock_trywrlock
#define rwl_wrunlock pthread_rwlock_unlock

#else /* HAVE_PTHREAD_RWLOCK */

/*
 * Structure describing a read-write lock.
 */
typedef struct rwlock_tag {
    pthread_mutex_t     mutex;
    pthread_cond_t      read;           /* wait for read */
    pthread_cond_t      write;          /* wait for write */
    int                 valid;          /* set when valid */
    int                 r_active;       /* readers active */
    int                 w_active;       /* writer active */
    int                 r_wait;         /* readers waiting */
    int                 w_wait;         /* writers waiting */
} rwlock_t;

#define RWLOCK_VALID    0xfacade

/*
 * Support static initialization of barriers
 */
#define RWL_INITIALIZER \
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
    PTHREAD_COND_INITIALIZER, RWLOCK_VALID, 0, 0, 0, 0}

/*
 * Define read-write lock functions
 */
extern int rwl_init (rwlock_t *rwlock);
extern int rwl_destroy (rwlock_t *rwlock);
extern int rwl_rdlock (rwlock_t *rwlock);
extern int rwl_tryrdlock (rwlock_t *rwlock);
extern int rwl_rdunlock (rwlock_t *rwlock);
extern int rwl_wrlock (rwlock_t *rwlock);
extern int rwl_trywrlock (rwlock_t *rwlock);
extern int rwl_wrunlock (rwlock_t *rwlock);

#endif  /* not HAVE_POSIX_RWLOCK */

#endif /* RWLOCK_H */

