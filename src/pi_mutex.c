// SPDX-License-Identifier: LGPL-2.1-only
// Copyright © 2018 VMware, Inc. All Rights Reserved.

#include "rtpi.h"
#include "pi_futex.h"
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

static pthread_once_t run_once = PTHREAD_ONCE_INIT;
static __thread pid_t tid_this_thread;

static void cleartid(void)
{
	tid_this_thread = 0;
}

static void install_atfork_handler()
{
	pthread_atfork(NULL, NULL, cleartid);
}

static pid_t gettid(void)
{
	if (tid_this_thread)
		return tid_this_thread;

	tid_this_thread = syscall(SYS_gettid);
	return tid_this_thread;
}

pi_mutex_t *pi_mutex_alloc(void)
{
	return malloc(sizeof(pi_mutex_t));
}

void pi_mutex_free(pi_mutex_t *mutex)
{
	free(mutex);
}

int pi_mutex_init(pi_mutex_t *mutex, uint32_t flags)
{
	int ret;

	ret = pthread_once(&run_once, install_atfork_handler);
	if (ret)
		goto out;

	/* All RTPI mutexes are PRIO_INHERIT */
	memset(mutex, 0, sizeof(*mutex));

	/* Check for unknown options */
	if (flags & ~(RTPI_MUTEX_PSHARED)) {
		ret = EINVAL;
		goto out;
	}

	if (flags & RTPI_MUTEX_PSHARED)
		mutex->flags = RTPI_MUTEX_PSHARED;
	ret = 0;
out:
	return ret;
}

int pi_mutex_destroy(pi_mutex_t *mutex)
{
	memset(mutex, 0, sizeof(*mutex));
	return 0;
}

int pi_mutex_lock(pi_mutex_t *mutex)
{
	int ret;

	if (!mutex)
		return EINVAL;

	ret = pi_mutex_trylock(mutex);
	if (!ret || ret == EDEADLOCK)
		return ret;
	return (futex_lock_pi(mutex)) ? errno : 0;
}

#define FUTEX_TID_MASK          0x3fffffff

int pi_mutex_trylock(pi_mutex_t *mutex)
{
	pid_t pid;
	bool ret;

	pid = gettid();
	if (pid == (mutex->futex & FUTEX_TID_MASK))
		return EDEADLOCK;

	ret = __sync_bool_compare_and_swap(&mutex->futex,
					   0, pid);
	return (ret) ? 0 : EBUSY;
}

int pi_mutex_unlock(pi_mutex_t *mutex)
{
	pid_t pid;
	bool ret;

	if (!mutex)
		return EINVAL;

	pid = gettid();
	if (pid != (mutex->futex & FUTEX_TID_MASK))
		return EPERM;

	ret = __sync_bool_compare_and_swap(&mutex->futex,
					   pid, 0);
	if (ret == true)
		return 0;
	return (futex_unlock_pi(mutex)) ? errno : 0;
}
