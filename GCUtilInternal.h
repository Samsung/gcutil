/*
 * Copyright (c) 2021-present Samsung Electronics Co., Ltd
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
*/

#ifndef __GCUtilInternal__
#define __GCUtilInternal__

#if defined(GC_THREAD_ISOLATE)

#if defined(_MSC_VER)
#define MAY_THREAD_LOCAL __declspec(thread)
#else
#define MAY_THREAD_LOCAL __thread
#endif

#else /* GC_THREAD_ISOLATE */

#define MAY_THREAD_LOCAL
#endif /* GC_THREAD_ISOLATE */

#endif
