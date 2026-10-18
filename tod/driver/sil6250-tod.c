/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define FP_COMPONENT "sil6250"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <gmodule.h>

#include <fp-device.h>
#include <fp-print.h>
#include <drivers_api.h>

#include "sil6250-tod.h"

#define ENROLL_STAGES 15
#define VERIFY_THRESHOLD 5
#define DRIVER_ID "sil6250"
#define DEVICE_LABEL "Huawei MateBook D16 SIL6250"

extern void *sil6250_session_new(const char *path);
extern void sil6250_session_free(void *session);
extern int sil6250_capture_features(void *session,
                                   uint32_t finger_timeout_ms,
                                   bool enroll_mode,
                                   uint8_t **out_data,
                                   size_t *out_len);
extern bool sil6250_wait_finger_up(void *session, uint32_t timeout_ms);
extern int sil6250_match_features(const uint8_t *probe,
                                  size_t probe_len,
                                  const uint8_t *enrolled,
                                  size_t enrolled_len);
extern void sil6250_bytes_free(uint8_t *data, size_t len);

struct _FpiDeviceSil6250 {
  FpDevice parent;
};

typedef struct {
  gchar *devpath;
} FpiDeviceSil6250Private;

struct _FpiDeviceSil6250Class {
  FpDeviceClass parent_class;
};

static void fpi_device_sil6250_class_init(FpiDeviceSil6250Class *klass);
static void fpi_device_sil6250_init(FpiDeviceSil6250 *self);
G_DEFINE_TYPE_WITH_PRIVATE(FpiDeviceSil6250, fpi_device_sil6250, FP_TYPE_DEVICE)

typedef struct {
  FpDevice *device;
  guint completed;
} EnrollProgress;

typedef struct {
  GPtrArray *blobs; /* GBytes* returned by Rust */
  gboolean matched;
  FpPrint *probe;
  FpPrint *matched_print;
} OperationResult;

typedef struct {
  FpPrint *enroll_print;
  FpPrint *verify_print;
  GPtrArray *gallery; /* FpPrint* refs for identify */
} OperationData;

typedef struct {
  guint8 *data;
  gsize len;
} RustBytes;

static void
rust_bytes_free(gpointer user_data)
{
  RustBytes *owned = user_data;
  sil6250_bytes_free(owned->data, owned->len);
  g_free(owned);
}

static FpiDeviceSil6250Private *
get_priv(FpDevice *device)
{
  return fpi_device_sil6250_get_instance_private((FpiDeviceSil6250 *)device);
}

G_MODULE_EXPORT GType
fpi_tod_shared_driver_get_type(void)
{
  return fpi_device_sil6250_get_type();
}

static const FpIdEntry sil6250_id_table[] = {
  { .virtual_envvar = "FP_SIL6250", .driver_data = 0 },
  { .pid = 0, .vid = 0 }
};

static void
sil6250_probe(FpDevice *device)
{
  const gchar *devpath = fpi_device_get_virtual_env(device);
  FpiDeviceSil6250Private *priv = get_priv(device);

  if (!devpath || devpath[0] != '/') {
    fpi_device_probe_complete(device, NULL, NULL,
                              fpi_device_error_new_msg(FP_DEVICE_ERROR_DATA_INVALID,
                                                       "FP_SIL6250 must be an absolute device path"));
    return;
  }

  g_free(priv->devpath);
  priv->devpath = g_strdup(devpath);
  fpi_device_probe_complete(device, "sil6250-64x80-v1", DEVICE_LABEL, NULL);
}

static void
sil6250_open(FpDevice *device)
{
  FpiDeviceSil6250Private *priv = get_priv(device);
  if (!priv->devpath || access(priv->devpath, R_OK | W_OK) != 0) {
    fpi_device_open_complete(device,
                             fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                                      "cannot access SIL6250 device: %s",
                                                      g_strerror(errno)));
    return;
  }
  fpi_device_open_complete(device, NULL);
}

static void
sil6250_close(FpDevice *device)
{
  fpi_device_close_complete(device, NULL);
}

static gboolean
enroll_progress_in_main(gpointer user_data)
{
  EnrollProgress *progress = user_data;
  fpi_device_enroll_progress(progress->device, progress->completed, NULL, NULL);
  g_object_unref(progress->device);
  g_free(progress);
  return G_SOURCE_REMOVE;
}

static void
post_enroll_progress(FpDevice *device, guint completed)
{
  EnrollProgress *progress = g_new0(EnrollProgress, 1);
  progress->device = g_object_ref(device);
  progress->completed = completed;
  g_main_context_invoke(NULL, enroll_progress_in_main, progress);
}

static GBytes *
capture_blob(void *session, GCancellable *cancellable, gboolean enroll_mode)
{
  guint8 *data = NULL;
  gsize len = 0;
  for (;;) {
    if (g_cancellable_is_cancelled(cancellable))
      return NULL;
    int rc = sil6250_capture_features(session, 1500, enroll_mode, &data, &len);
    if (rc == 0) {
      RustBytes *allocation = g_new0(RustBytes, 1);
      allocation->data = data;
      allocation->len = len;
      return g_bytes_new_with_free_func(data, len, rust_bytes_free, allocation);
    }
    if (rc < 0)
      return NULL;
    /* Poor or redundant captures are retried only after a lift. */
    sil6250_wait_finger_up(session, 2500);
  }
}

static void
operation_data_free(gpointer user_data)
{
  OperationData *data = user_data;
  g_clear_object(&data->enroll_print);
  g_clear_object(&data->verify_print);
  g_clear_pointer(&data->gallery, g_ptr_array_unref);
  g_free(data);
}

static void
operation_result_free(gpointer user_data)
{
  OperationResult *result = user_data;
  g_clear_pointer(&result->blobs, g_ptr_array_unref);
  g_clear_object(&result->probe);
  g_clear_object(&result->matched_print);
  g_free(result);
}

static void
enroll_worker(GTask *task,
              gpointer source_object,
              gpointer task_data,
              GCancellable *cancellable)
{
  (void)task_data;
  FpDevice *device = FP_DEVICE(source_object);
  FpiDeviceSil6250Private *priv = get_priv(device);
  OperationResult *result = g_new0(OperationResult, 1);
  result->blobs = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

  void *session = sil6250_session_new(priv->devpath);
  if (!session) {
    operation_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot open SIL6250 session");
    return;
  }

  while (result->blobs->len < ENROLL_STAGES) {
    if (g_task_return_error_if_cancelled(task)) {
      sil6250_session_free(session);
      operation_result_free(result);
      return;
    }
    GBytes *blob = capture_blob(session, cancellable, TRUE);
    if (!blob) {
      if (g_cancellable_is_cancelled(cancellable)) {
        sil6250_session_free(session);
        operation_result_free(result);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "enrollment cancelled");
      } else {
        sil6250_session_free(session);
        operation_result_free(result);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "SIL6250 capture failed during enrollment");
      }
      return;
    }
    g_ptr_array_add(result->blobs, blob);
    post_enroll_progress(device, result->blobs->len);
    if (result->blobs->len < ENROLL_STAGES)
      sil6250_wait_finger_up(session, 5000);
  }

  sil6250_session_free(session);
  g_task_return_pointer(task, result, operation_result_free);
}

static GVariant *
serialize_blobs(GPtrArray *blobs)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aay"));
  for (guint i = 0; i < blobs->len; i++) {
    GBytes *bytes = g_ptr_array_index(blobs, i);
    gsize len;
    gconstpointer data = g_bytes_get_data(bytes, &len);
    GVariant *array = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1);
    g_variant_builder_add_value(&builder, array);
  }
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void
enroll_done(GObject *source_object, GAsyncResult *async_result, gpointer user_data)
{
  (void)user_data;
  FpDevice *device = FP_DEVICE(source_object);
  OperationData *data = g_task_get_task_data(G_TASK(async_result));
  g_autoptr(GError) error = NULL;
  OperationResult *result = g_task_propagate_pointer(G_TASK(async_result), &error);

  if (!result) {
    fpi_device_enroll_complete(device, NULL,
                               fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                                        "SIL6250 enrollment failed: %s",
                                                        error ? error->message : "unknown error"));
    return;
  }

  FpPrint *print = data->enroll_print;
  fpi_print_set_type(print, FPI_PRINT_RAW);
  g_autoptr(GVariant) variant = serialize_blobs(result->blobs);
  g_object_set(print, "fpi-data", variant, NULL);
  /* enroll_complete() consumes this reference. */
  data->enroll_print = NULL;
  fpi_device_enroll_complete(device, print, NULL);
  operation_result_free(result);
}

static void
sil6250_enroll(FpDevice *device)
{
  OperationData *data = g_new0(OperationData, 1);
  fpi_device_get_enroll_data(device, &data->enroll_print);
  if (!data->enroll_print) {
    g_free(data);
    fpi_device_enroll_complete(device, NULL,
                               fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID));
    return;
  }
  g_object_ref(data->enroll_print);
  GTask *task = g_task_new(device, fpi_device_get_cancellable(device), enroll_done, NULL);
  g_task_set_task_data(task, data, operation_data_free);
  g_task_run_in_thread(task, enroll_worker);
  g_object_unref(task);
}

static GVariant *
get_print_data(FpPrint *print)
{
  GVariant *variant = NULL;
  g_object_get(print, "fpi-data", &variant, NULL);
  return variant;
}

static gboolean
match_probe_to_print(GBytes *probe,
                     FpPrint *print,
                     GCancellable *cancellable)
{
  g_autoptr(GVariant) variant = get_print_data(print);
  if (!variant || !g_variant_is_of_type(variant, G_VARIANT_TYPE("aay")))
    return FALSE;

  gsize probe_len;
  gconstpointer probe_data = g_bytes_get_data(probe, &probe_len);
  GVariantIter iter;
  GVariant *item;
  g_variant_iter_init(&iter, variant);
  while ((item = g_variant_iter_next_value(&iter))) {
    if (g_cancellable_is_cancelled(cancellable)) {
      g_variant_unref(item);
      return FALSE;
    }
    gsize template_len;
    gconstpointer template_data = g_variant_get_fixed_array(item, &template_len, 1);
    int score = sil6250_match_features(probe_data, probe_len,
                                       template_data, template_len);
    g_variant_unref(item);
    if (score >= VERIFY_THRESHOLD)
      return TRUE;
  }
  return FALSE;
}

static void
verify_worker(GTask *task,
              gpointer source_object,
              gpointer task_data,
              GCancellable *cancellable)
{
  FpDevice *device = FP_DEVICE(source_object);
  FpiDeviceSil6250Private *priv = get_priv(device);
  OperationData *data = task_data;
  OperationResult *result = g_new0(OperationResult, 1);
  GPtrArray *prints = data->gallery ? data->gallery : NULL;

  if (!prints && data->verify_print) {
    prints = g_ptr_array_new();
    g_ptr_array_add(prints, data->verify_print);
  }
  /* libfprint calls identify with an empty gallery before enrollment to
   * check for duplicate prints. It still expects a scanned print in that
   * case, so only a missing verification target is an error. */
  if (!prints || (!data->gallery && !data->verify_print)) {
    if (!data->gallery && prints)
      g_ptr_array_unref(prints);
    operation_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "no SIL6250 fingerprint was selected for verification");
    return;
  }

  void *session = sil6250_session_new(priv->devpath);
  if (!session) {
    if (!data->gallery)
      g_ptr_array_unref(prints);
    operation_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot open SIL6250 session");
    return;
  }

  GBytes *probe = capture_blob(session, cancellable, FALSE);
  if (!probe) {
    sil6250_session_free(session);
    if (!data->gallery)
      g_ptr_array_unref(prints);
    operation_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR,
                            g_cancellable_is_cancelled(cancellable)
                                ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
                            "SIL6250 capture failed during verification");
    return;
  }

  for (guint i = 0; i < prints->len; i++) {
    FpPrint *candidate = g_ptr_array_index(prints, i);
    if (match_probe_to_print(probe, candidate, cancellable)) {
      result->matched = TRUE;
      result->matched_print = g_object_ref(candidate);
      break;
    }
  }
  result->probe = fp_print_new(device);
  fpi_print_set_type(result->probe, FPI_PRINT_RAW);
  gsize probe_len;
  gconstpointer probe_data = g_bytes_get_data(probe, &probe_len);
  GVariant *probe_array = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                    probe_data, probe_len, 1);
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aay"));
  g_variant_builder_add_value(&builder, probe_array);
  g_autoptr(GVariant) probe_variant = g_variant_ref_sink(g_variant_builder_end(&builder));
  g_object_set(result->probe, "fpi-data", probe_variant, NULL);
  g_bytes_unref(probe);
  sil6250_session_free(session);
  if (!data->gallery)
    g_ptr_array_unref(prints);
  g_task_return_pointer(task, result, operation_result_free);
}

static void
verify_done(GObject *source_object, GAsyncResult *async_result, gpointer user_data)
{
  (void)user_data;
  FpDevice *device = FP_DEVICE(source_object);
  g_autoptr(GError) error = NULL;
  OperationResult *result = g_task_propagate_pointer(G_TASK(async_result), &error);
  if (!result) {
    fpi_device_verify_complete(device,
                               fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                                        "SIL6250 verification failed: %s",
                                                        error ? error->message : "unknown error"));
    return;
  }
  FpPrint *probe = result->probe;
  result->probe = NULL; /* verify_report() consumes the scanned print */
  fpi_device_verify_report(device,
                           result->matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                           probe, NULL);
  fpi_device_verify_complete(device, NULL);
  operation_result_free(result);
}

static void
identify_done(GObject *source_object, GAsyncResult *async_result, gpointer user_data)
{
  (void)user_data;
  FpDevice *device = FP_DEVICE(source_object);
  g_autoptr(GError) error = NULL;
  OperationResult *result = g_task_propagate_pointer(G_TASK(async_result), &error);
  if (!result) {
    fpi_device_identify_complete(device,
                                 fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                                          "SIL6250 identification failed: %s",
                                                          error ? error->message : "unknown error"));
    return;
  }
  FpPrint *probe = result->probe;
  result->probe = NULL; /* identify_report() consumes the scanned print */
  fpi_device_identify_report(device, result->matched_print, probe, NULL);
  fpi_device_identify_complete(device, NULL);
  operation_result_free(result);
}

static void
sil6250_verify(FpDevice *device)
{
  OperationData *data = g_new0(OperationData, 1);
  fpi_device_get_verify_data(device, &data->verify_print);
  if (data->verify_print)
    g_object_ref(data->verify_print);
  GTask *task = g_task_new(device, fpi_device_get_cancellable(device), verify_done, NULL);
  g_task_set_task_data(task, data, operation_data_free);
  g_task_run_in_thread(task, verify_worker);
  g_object_unref(task);
}

static void
sil6250_identify(FpDevice *device)
{
  OperationData *data = g_new0(OperationData, 1);
  GPtrArray *prints = NULL;
  fpi_device_get_identify_data(device, &prints);
  if (prints) {
    GPtrArray *refs = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < prints->len; i++)
      g_ptr_array_add(refs, g_object_ref(g_ptr_array_index(prints, i)));
    data->gallery = refs;
  }
  GTask *task = g_task_new(device, fpi_device_get_cancellable(device), identify_done, NULL);
  g_task_set_task_data(task, data, operation_data_free);
  g_task_run_in_thread(task, verify_worker);
  g_object_unref(task);
}

static void
sil6250_cancel(FpDevice *device)
{
  (void)device;
  /* The active operation observes libfprint's GCancellable between bounded
   * captures. No second thread touches the sensor. */
}

static void
fpi_device_sil6250_init(FpiDeviceSil6250 *self)
{
  (void)self;
}

static void
fpi_device_sil6250_class_init(FpiDeviceSil6250Class *klass)
{
  FpDeviceClass *device_class = FP_DEVICE_CLASS(klass);
  device_class->id = DRIVER_ID;
  device_class->full_name = DEVICE_LABEL;
  device_class->type = FP_DEVICE_TYPE_VIRTUAL;
  device_class->id_table = sil6250_id_table;
  device_class->nr_enroll_stages = ENROLL_STAGES;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;
  device_class->probe = sil6250_probe;
  device_class->open = sil6250_open;
  device_class->close = sil6250_close;
  device_class->enroll = sil6250_enroll;
  device_class->verify = sil6250_verify;
  device_class->identify = sil6250_identify;
  device_class->cancel = sil6250_cancel;
  fpi_device_class_auto_initialize_features(device_class);
}

