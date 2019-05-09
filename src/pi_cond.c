// SPDX-License-Identifier: LGPL-2.1-only
// Copyright © 2018 VMware, Inc. All Rights Reserved.

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include "rtpi.h"
#include "pi_futex.h"

/*
 * This wrapper for early library validation only.
 * TODO: Replace with pthread_cond_t wrapper with a new cond implementation.
 *       Base this on the older version of the condvar, with the patch from
 *       Dinakar and Darren to enable priority fifo wakeup order.
 */

pi_cond_t *pi_cond_alloc(void)
{
	return malloc(sizeof(pi_cond_t));
}

void pi_cond_free(pi_cond_t *cond)
{
	free(cond);
}

int pi_cond_init(pi_cond_t *cond, uint32_t flags)
{
	struct timespec ts = { 0, 0 };
	int ret;

	if (flags & ~(RTPI_COND_PSHARED)) {
		ret = EINVAL;
		goto out;
	}
	memset(cond, 0, sizeof(*cond));
	if (flags & RTPI_COND_PSHARED) {
		cond->flags = RTPI_COND_PSHARED;
	}

	pi_mutex_init(&cond->priv_mut, cond->flags & RTPI_COND_PSHARED);

	ret = 0;
out:
	return ret;
}

int pi_cond_destroy(pi_cond_t *cond)
{
	memset(cond, 0, sizeof(*cond));
	return 0;
}

int pi_cond_timedwait(pi_cond_t *cond, pi_mutex_t *mutex,
		      const struct timespec *restrict abstime)
{
	int ret;
	int err;
	__u32 wait_id;
	__u32 futex_id;

	ret = pi_mutex_lock(&cond->priv_mut);
	if (ret)
		return ret;

	ret = pi_mutex_unlock(mutex);
	if (ret) {
		pi_mutex_unlock(&cond->priv_mut);
		return ret;
	}

	cond->cond++;
	wait_id = cond->cond;
  again:
	futex_id = cond->cond;
	pi_mutex_unlock(&cond->priv_mut);

	ret = futex_wait_requeue_pi(cond, futex_id, abstime, mutex);

	/* All good. Proper wakeup + we own the lock */
	if (!ret)
		return 0;

	if (errno == EAGAIN) {
		/* futex VAL changed between unlock & wait */
		pi_mutex_lock(&cond->priv_mut);
		if (cond->wake_id >= wait_id) {
			/* There is one wakeup pending for us */
			pi_mutex_unlock(&cond->priv_mut);
			pi_mutex_lock(mutex);
			return 0;
		}
		/* Reload VAL and try again */
		cond->cond++;
		goto again;
	}

	/* Error, grab the user mutex before returning */
	pi_mutex_lock(mutex);
	return errno;
}

int pi_cond_wait(pi_cond_t *cond, pi_mutex_t *mutex)
{
	return pi_cond_timedwait(cond, mutex, NULL);
}

static int pi_cond_signal_common(pi_cond_t *cond, pi_mutex_t *mutex, bool broadcast)
{
	int ret;
	__u32 id;

again:
	pi_mutex_lock(&cond->priv_mut);
	cond->cond++;
	id = cond->cond;
	cond->wake_id = id;
	pi_mutex_unlock(&cond->priv_mut);

	ret = futex_cmp_requeue_pi(cond, id,
				   (broadcast) ? INT_MAX : 0,
				   mutex);
	if (ret >= 0)
		return 0;

	if (errno == EAGAIN)
		goto again;

	return errno;
}

int pi_cond_broadcast(pi_cond_t *cond, pi_mutex_t *mutex)
{
	return pi_cond_signal_common(cond, mutex, true);
}

int pi_cond_signal(pi_cond_t *cond, pi_mutex_t *mutex)
{
	return pi_cond_signal_common(cond, mutex, false);
}
