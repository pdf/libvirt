/*
 * threads.c: basic thread synchronization primitives
 *
 * Copyright (C) 2009, 2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include "internal.h"

#include <pthread.h>

struct virMutex {
    pthread_mutex_t lock;
};

struct virCond {
    pthread_cond_t cond;
};

struct virThread {
    pthread_t thread;
};

struct virThreadLocal {
    pthread_key_t key;
};

struct virOnceControl {
    pthread_once_t once;
};

#define VIR_ONCE_CONTROL_INITIALIZER \
{                                    \
    .once = PTHREAD_ONCE_INIT        \
}
