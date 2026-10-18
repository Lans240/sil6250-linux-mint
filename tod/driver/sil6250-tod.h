/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _FpiDeviceSil6250 FpiDeviceSil6250;
typedef struct _FpiDeviceSil6250Class FpiDeviceSil6250Class;

#define FPI_TYPE_DEVICE_SIL6250 (fpi_device_sil6250_get_type())
GType fpi_device_sil6250_get_type(void);

G_END_DECLS
