// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Jesus Higueras <jesus@furilabs.com>
// Copyright (C) 2024 Bardia Moshiri <bardia@furilabs.com>

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <inttypes.h>
#include <batman/wlrdisplay.h>
#include "virtkey.h"
#include <signal.h>

#ifdef G_LOG_DOMAIN
#undef G_LOG_DOMAIN
#endif
#define G_LOG_DOMAIN "GestureSensors"

typedef struct {
    GDBusConnection *dbus_connection;
    gint32 wake_session_id;
    gint32 tilt_session_id;
    GMainLoop *main_loop;
    gboolean previous_screen_on;
    GSettings *settings;
    gchar *logind_session_id;
    guint subscription_id;
    guint idle_source_id;
} GestureSensors;

static GestureSensors *g_app = NULL;

void
send_wake_key()
{
    struct wtype wtype;
    memset(&wtype, 0, sizeof(wtype));

    wtype.commands = calloc(1, sizeof(wtype.commands[0]));
    wtype.command_count = 1;

    struct wtype_command *cmd = &wtype.commands[0];
    cmd->type = WTYPE_COMMAND_TEXT;
    xkb_keysym_t ks = xkb_keysym_from_name("Escape", XKB_KEYSYM_CASE_INSENSITIVE);
    if (ks == XKB_KEY_NoSymbol) {
        g_printerr("Unknown key 'Escape'");
        return;
    }

    cmd->key_codes = malloc(sizeof(cmd->key_codes[0]));
    cmd->key_codes_len = 1;
    cmd->key_codes[0] = get_key_code_by_xkb(&wtype, ks);
    cmd->delay_ms = 0;

    wtype.display = wl_display_connect(NULL);
    if (wtype.display == NULL) {
        g_printerr("Wayland connection failed\n");
        return;
    }
    wtype.registry = wl_display_get_registry(wtype.display);
    wl_registry_add_listener(wtype.registry, &registry_listener, &wtype);
    wl_display_dispatch(wtype.display);
    wl_display_roundtrip(wtype.display);

    if (wtype.manager == NULL) {
        g_printerr("Compositor does not support the virtual keyboard protocol\n");
        return;
    }
    if (wtype.seat == NULL) {
        g_printerr("No seat found\n");
        return;
    }

    wtype.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        wtype.manager, wtype.seat
    );

    upload_keymap(&wtype);
    run_commands(&wtype);

    g_debug("Escape key sent to seat");

    free(wtype.commands);
    free(wtype.keymap);
    zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
    zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
    wl_registry_destroy(wtype.registry);
    wl_display_disconnect(wtype.display);
}

gint32
request_wake_sensor(GestureSensors *app)
{
    GVariant *result;
    GError *error = NULL;
    gint32 session_id = -1;

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "loadPlugin",
                                         g_variant_new("(s)", "wakegesturesensor"),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to load plugin: %s", error->message);
        g_error_free(error);
        return -1;
    }

    g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "requestSensor",
                                         g_variant_new("(sx)", "wakegesturesensor", (gint64)getpid()),
                                         G_VARIANT_TYPE("(i)"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to request sensor: %s", error->message);
        g_error_free(error);
        return -1;
    }

    g_variant_get(result, "(i)", &session_id);
    g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/wakegesturesensor",
                                         "local.WakeGestureSensor",
                                         "start",
                                         g_variant_new("(i)", session_id),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to start sensor: %s", error->message);
        g_error_free(error);
    }

    g_variant_unref(result);

    return session_id;
}

void
release_wake_sensor(GestureSensors *app, gint32 session_id)
{
    GVariant *result;
    GError *error = NULL;

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/wakegesturesensor",
                                         "local.WakeGestureSensor",
                                         "stop",
                                         g_variant_new("(i)", session_id),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to stop sensor: %s", error->message);
        g_error_free(error);
    }

    if (result)
        g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "releaseSensor",
                                         g_variant_new("(six)", "wakegesturesensor", session_id, (gint64)getpid()),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to release sensor: %s", error->message);
        g_error_free(error);
    }

    if (result)
        g_variant_unref(result);
}

guint32
get_wake_sensor_reading(GestureSensors *app)
{
    GVariant *result;
    GError *error = NULL;
    guint32 wake_gesture = 0;
    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/wakegesturesensor",
                                         "org.freedesktop.DBus.Properties",
                                         "Get",
                                         g_variant_new("(ss)", "local.WakeGestureSensor", "wakegesture"),
                                         G_VARIANT_TYPE("(v)"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to get sensor reading: %s", error->message);
        g_error_free(error);
        return 0;
    }

    GVariant *value;
    g_variant_get(result, "(v)", &value);
    g_variant_get(value, "(tu)", NULL, &wake_gesture);

    g_variant_unref(value);
    g_variant_unref(result);

    return wake_gesture;
}

gint32
request_tilt_sensor(GestureSensors *app)
{
    GVariant *result;
    GError *error = NULL;
    gint32 session_id = -1;

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "loadPlugin",
                                         g_variant_new("(s)", "tiltdetectorsensor"),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to load tilt sensor plugin: %s", error->message);
        g_error_free(error);
        return -1;
    }

    g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "requestSensor",
                                         g_variant_new("(sx)", "tiltdetectorsensor", (gint64)getpid()),
                                         G_VARIANT_TYPE("(i)"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to request tilt sensor: %s", error->message);
        g_error_free(error);
        return -1;
    }

    g_variant_get(result, "(i)", &session_id);
    g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/tiltdetectorsensor",
                                         "local.TiltDetectorSensor",
                                         "start",
                                         g_variant_new("(i)", session_id),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to start tilt sensor: %s", error->message);
        g_error_free(error);
    }

    g_variant_unref(result);

    return session_id;
}

void
release_tilt_sensor(GestureSensors *app, gint32 session_id)
{
    GVariant *result;
    GError *error = NULL;

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/tiltdetectorsensor",
                                         "local.TiltDetectorSensor",
                                         "stop",
                                         g_variant_new("(i)", session_id),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to stop tilt sensor: %s", error->message);
        g_error_free(error);
    }

    if (result)
        g_variant_unref(result);

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager",
                                         "local.SensorManager",
                                         "releaseSensor",
                                         g_variant_new("(six)", "tiltdetectorsensor", session_id, (gint64)getpid()),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to release tilt sensor: %s", error->message);
        g_error_free(error);
    }

    if (result)
        g_variant_unref(result);
}

guint32
get_tilt_sensor_reading(GestureSensors *app)
{
    GVariant *result;
    GError *error = NULL;
    guint32 tilt_detected = 0;
    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "com.nokia.SensorService",
                                         "/SensorManager/tiltdetectorsensor",
                                         "org.freedesktop.DBus.Properties",
                                         "Get",
                                         g_variant_new("(ss)", "local.TiltDetectorSensor", "tiltdetector"),
                                         G_VARIANT_TYPE("(v)"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);
    if (error) {
        g_warning("Failed to get tilt sensor reading: %s", error->message);
        g_error_free(error);
        return 0;
    }

    GVariant *value;
    g_variant_get(result, "(v)", &value);
    g_variant_get(value, "(tu)", NULL, &tilt_detected);

    g_variant_unref(value);
    g_variant_unref(result);

    return tilt_detected;
}

static gchar*
get_session_id(GestureSensors *app)
{
    GVariant *result;
    GError *error = NULL;
    gchar *session_id = NULL;

    result = g_dbus_connection_call_sync(app->dbus_connection,
                                         "org.freedesktop.login1",
                                         "/org/freedesktop/login1",
                                         "org.freedesktop.login1.Manager",
                                         "ListSessions",
                                         NULL,
                                         G_VARIANT_TYPE("(a(susso))"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);

    if (error) {
        g_warning("Failed to list sessions: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    GVariantIter *iter;
    gchar *id, *seat, *path;

    g_variant_get(result, "(a(susso))", &iter);
    while (g_variant_iter_loop(iter, "(susso)", &id, NULL, NULL, &seat, &path)) {
        if (g_strcmp0(seat, "seat0") == 0) {
            session_id = g_strdup(id);
            break;
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(result);

    return session_id;
}

static void
handle_wake_gesture(GestureSensors *app)
{
    GError *error = NULL;

    g_dbus_connection_call_sync(app->dbus_connection,
                                "com.nokia.SensorService",
                                "/SensorManager/wakegesturesensor",
                                "local.WakeGestureSensor",
                                "resetWakeGesture",
                                NULL,
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                &error);

    g_dbus_connection_call_sync(app->dbus_connection,
                                "com.nokia.SensorService",
                                "/SensorManager/tiltdetectorsensor",
                                "local.TiltDetectorSensor",
                                "resetTiltDetector",
                                NULL,
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                &error);
    if (error) {
        g_warning("Failed to reset sensors: %s", error->message);
        g_error_free(error);
    }

    release_wake_sensor(app, app->wake_session_id);
    release_tilt_sensor(app, app->tilt_session_id);
    app->wake_session_id = request_wake_sensor(app);
    app->tilt_session_id = request_tilt_sensor(app);
    if (app->wake_session_id == -1 || app->tilt_session_id == -1) {
        g_printerr("Failed to request new sensors after reset\n");
        g_main_loop_quit(app->main_loop);
        return;
    }

    send_wake_key();
}

static gboolean
check_sensors(gpointer user_data)
{
    GestureSensors *app = (GestureSensors *)user_data;

    int result = wlrdisplay(0, NULL);
    gboolean current_screen_on = (result == 0);
    if (current_screen_on) {
        g_debug("Screen is on, stopping sensor checks");
        app->idle_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    gboolean wake_enabled = g_settings_get_boolean(app->settings, "wake-sensor-enabled");
    gboolean tilt_enabled = g_settings_get_boolean(app->settings, "tilt-sensor-enabled");
    if (!wake_enabled && !tilt_enabled) {
        g_debug("All sensors disabled, stopping checks");
        app->idle_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    guint32 wake_reading = wake_enabled ? get_wake_sensor_reading(app) : 0;
    guint32 tilt_reading = tilt_enabled ? get_tilt_sensor_reading(app) : 0;
    if (wake_reading == 1 || tilt_reading == 1) {
        g_debug("Wake gesture or tilt detected! Wake: %u, Tilt: %u", wake_reading, tilt_reading);
        handle_wake_gesture(app);
        app->idle_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    g_usleep(500000);

    return G_SOURCE_CONTINUE;
}

static void
on_idle_hint_changed(GDBusConnection *connection,
                     const gchar *sender_name,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
    GestureSensors *app = (GestureSensors *)user_data;
    const gchar *property_interface;
    GVariant *changed_properties;
    GVariant *invalidated_properties;

    g_variant_get(parameters, "(&s@a{sv}@as)",
                  &property_interface,
                  &changed_properties,
                  &invalidated_properties);

    GVariant *idle_variant = g_variant_lookup_value(changed_properties, "IdleHint", G_VARIANT_TYPE_BOOLEAN);
    if (idle_variant) {
        gboolean idle = g_variant_get_boolean(idle_variant);
        g_debug("IdleHint changed: %d", idle);

        gboolean wake_enabled = g_settings_get_boolean(app->settings, "wake-sensor-enabled");
        gboolean tilt_enabled = g_settings_get_boolean(app->settings, "tilt-sensor-enabled");

        if (idle && app->idle_source_id == 0 && (wake_enabled || tilt_enabled)) {
            g_debug("Screen turned off, releasing and requesting sensors");
            release_wake_sensor(app, app->wake_session_id);
            release_tilt_sensor(app, app->tilt_session_id);
            app->wake_session_id = request_wake_sensor(app);
            app->tilt_session_id = request_tilt_sensor(app);
            if (app->wake_session_id == -1 || app->tilt_session_id == -1) {
                g_printerr("Failed to request new sensors after reset\n");
                g_main_loop_quit(app->main_loop);
                return;
            }

            g_debug("System went idle, starting sensor checks");
            app->idle_source_id = g_idle_add(check_sensors, app);
        }

        g_variant_unref(idle_variant);
    }

    g_variant_unref(changed_properties);
    g_variant_unref(invalidated_properties);
}

static void
subscribe_to_idle_hint(GestureSensors *app)
{
    GError *error = NULL;
    gchar *session_path;

    while (!app->logind_session_id) {
        app->logind_session_id = get_session_id(app);
        if (!app->logind_session_id) {
            g_warning("Failed to get session ID. Retrying...");
            g_usleep(1000000);
            continue;
        }
    }

    session_path = g_strdup_printf("/org/freedesktop/login1/session/%s", app->logind_session_id);

    app->subscription_id = g_dbus_connection_signal_subscribe(app->dbus_connection,
                                                              "org.freedesktop.login1",
                                                              "org.freedesktop.DBus.Properties",
                                                              "PropertiesChanged",
                                                              session_path,
                                                              NULL,
                                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                                              on_idle_hint_changed,
                                                              app,
                                                              NULL);

    g_debug("Listening for IdleHint changes on session %s", app->logind_session_id);
    g_free(session_path);
}

static void
cleanup_and_exit(GestureSensors *app)
{
    if (app->idle_source_id > 0) {
        g_source_remove(app->idle_source_id);
        app->idle_source_id = 0;
    }
    if (app->subscription_id > 0)
        g_dbus_connection_signal_unsubscribe(app->dbus_connection, app->subscription_id);
    if (app->wake_session_id != -1)
        release_wake_sensor(app, app->wake_session_id);
    if (app->tilt_session_id != -1)
        release_tilt_sensor(app, app->tilt_session_id);
    if (app->dbus_connection)
        g_object_unref(app->dbus_connection);
    if (app->settings)
        g_object_unref(app->settings);
    if (app->logind_session_id)
        g_free(app->logind_session_id);
    if (app->main_loop) {
        g_main_loop_quit(app->main_loop);
        g_main_loop_unref(app->main_loop);
    }
}

static void
signal_handler(int signum)
{
    if (!g_app)
        return;

    g_debug("Caught signal %d, cleaning up and exiting...", signum);

    if (g_app->main_loop)
        g_main_loop_quit(g_app->main_loop);
}

int
main(int argc, char *argv[])
{
    GestureSensors app = {0};
    GError *error = NULL;

    g_app = &app;

    struct sigaction sa = {
        .sa_handler = signal_handler,
        .sa_flags = SA_RESTART,
    };
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        g_printerr("Failed to set up signal handlers\n");
        return 1;
    }

    app.dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app.dbus_connection) {
        g_printerr("Failed to connect to D-Bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    app.settings = g_settings_new("io.furios.gesture");
    if (!app.settings) {
        g_printerr("Failed to create GSettings object\n");
        cleanup_and_exit(&app);
        return 1;
    }

    app.wake_session_id = request_wake_sensor(&app);
    app.tilt_session_id = request_tilt_sensor(&app);
    if (app.wake_session_id == -1 || app.tilt_session_id == -1) {
        g_printerr("Failed to request sensors\n");
        cleanup_and_exit(&app);
        return 1;
    }

    subscribe_to_idle_hint(&app);

    app.main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.main_loop);

    cleanup_and_exit(&app);

    g_app = NULL;

    return 0;
}
