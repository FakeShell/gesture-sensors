/**
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2025 Jesus Higueras <jesus@furilabs.com>
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <inttypes.h>
#include <batman/wlrdisplay.h>
#include "virtkey.h"
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef G_LOG_DOMAIN
#undef G_LOG_DOMAIN
#endif
#define G_LOG_DOMAIN "GestureSensors"

#define PALM_REJECTION_PATH "/sys/kernel/furilabs/palmrejectionmode/common_node/palmrejectionmode"
#define GLOVE_MODE_PATH "/sys/kernel/prize/glovemode/common_node/glovemode"

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
write_to_file(const char *path,
              const char *value)
{
    g_debug("Attempting to write to %s: %s\n", path, value);

    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("open");
        return;
    }

    if (write(fd, value, strlen(value)) == -1)
        perror("write");

    close(fd);
}

char *
read_from_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return NULL;

    char buffer[256];
    if (!fgets(buffer, sizeof(buffer), file)) {
        fclose(file);
        return NULL;
    }

    fclose(file);
    buffer[strcspn (buffer, "\n")] = '\0';
    return strdup(buffer);
}

void
send_wake_key(void)
{
    struct wtype wtype;
    struct wtype_command *cmd = NULL;
    xkb_keysym_t ks;

    memset(&wtype, 0, sizeof(wtype));

    wtype.commands = calloc(1, sizeof(wtype.commands[0]));
    if (!wtype.commands) {
        g_warning("Failed to allocate memory for commands");
        return;
    }
    wtype.command_count = 1;

    cmd = &wtype.commands[0];
    cmd->type = WTYPE_COMMAND_TEXT;
    ks = xkb_keysym_from_name("Escape", XKB_KEYSYM_CASE_INSENSITIVE);
    if (ks == XKB_KEY_NoSymbol) {
        g_warning("Unknown key 'Escape'");
        goto cleanup;
    }

    cmd->key_codes = malloc(sizeof(cmd->key_codes[0]));
    if (!cmd->key_codes) {
        g_warning("Failed to allocate memory for key codes");
        goto cleanup;
    }
    cmd->key_codes_len = 1;
    cmd->key_codes[0] = get_key_code_by_xkb(&wtype, ks);
    cmd->delay_ms = 0;

    wtype.display = wl_display_connect(NULL);
    if (wtype.display == NULL) {
        g_warning("Wayland connection failed");
        goto cleanup_key_codes;
    }

    wtype.registry = wl_display_get_registry(wtype.display);
    if (!wtype.registry) {
        g_warning("Failed to get Wayland registry");
        goto cleanup_display;
    }

    wl_registry_add_listener(wtype.registry, &registry_listener, &wtype);
    wl_display_dispatch(wtype.display);
    wl_display_roundtrip(wtype.display);

    if (wtype.manager == NULL) {
        g_warning("Compositor does not support the virtual keyboard protocol");
        goto cleanup_wayland_objects;
    }

    if (wtype.seat == NULL) {
        g_warning("No seat found");
        goto cleanup_wayland_objects;
    }

    wtype.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        wtype.manager, wtype.seat
    );
    if (!wtype.keyboard) {
        g_warning("Failed to create virtual keyboard");
        goto cleanup_wayland_objects;
    }

    upload_keymap(&wtype);
    run_commands(&wtype);

    g_debug("Escape key sent to seat");

    /* Cleanup virtual keyboard */
    if (wtype.keyboard) {
        zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
        wtype.keyboard = NULL;
    }

cleanup_wayland_objects:
    /* Clean up bound Wayland objects */
    if (wtype.manager) {
        zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
        wtype.manager = NULL;
    }
    if (wtype.seat) {
        wl_seat_destroy(wtype.seat);
        wtype.seat = NULL;
    }
    if (wtype.registry) {
        wl_registry_destroy(wtype.registry);
        wtype.registry = NULL;
    }

cleanup_display:
    if (wtype.display) {
        /* Flush and sync before disconnecting */
        wl_display_flush(wtype.display);
        wl_display_roundtrip(wtype.display);
        wl_display_disconnect(wtype.display);
        wtype.display = NULL;
    }
    if (wtype.keymap) {
        free(wtype.keymap);
        wtype.keymap = NULL;
    }

cleanup_key_codes:
    if (cmd && cmd->key_codes) {
        free(cmd->key_codes);
        cmd->key_codes = NULL;
    }

cleanup:
    if (wtype.commands) {
        free(wtype.commands);
        wtype.commands = NULL;
    }
}

gint32
request_wake_sensor(GestureSensors *app)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;
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
        return -1;
    }

    g_clear_pointer(&result, g_variant_unref);

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
        return -1;
    }

    g_variant_get(result, "(i)", &session_id);

    g_clear_pointer(&result, g_variant_unref);

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

    if (error)
        g_warning("Failed to start sensor: %s", error->message);

    return session_id;
}

void
release_wake_sensor(GestureSensors *app, gint32 session_id)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;

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
        g_clear_error(&error);
    }

    g_clear_pointer(&result, g_variant_unref);

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

    if (error)
        g_warning("Failed to release sensor: %s", error->message);
}

guint32
get_wake_sensor_reading(GestureSensors *app)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;
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
        return 0;
    }

    g_autoptr(GVariant) value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_get(value, "(tu)", NULL, &wake_gesture);

    return wake_gesture;
}

gint32
request_tilt_sensor(GestureSensors *app)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;
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
        return -1;
    }

    g_clear_pointer(&result, g_variant_unref);

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
        return -1;
    }

    g_variant_get(result, "(i)", &session_id);

    g_clear_pointer(&result, g_variant_unref);

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

    if (error)
        g_warning("Failed to start tilt sensor: %s", error->message);

    return session_id;
}

void
release_tilt_sensor(GestureSensors *app, gint32 session_id)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;

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
        g_clear_error(&error);
    }

    g_clear_pointer(&result, g_variant_unref);

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

    if (error)
        g_warning("Failed to release tilt sensor: %s", error->message);
}

guint32
get_tilt_sensor_reading(GestureSensors *app)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;
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
        return 0;
    }

    g_autoptr(GVariant) value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_get(value, "(tu)", NULL, &tilt_detected);

    return tilt_detected;
}

static gchar*
get_session_id(GestureSensors *app)
{
    g_autoptr(GVariant) result = NULL;
    g_autoptr(GError) error = NULL;
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
        return NULL;
    }

    GVariantIter *iter;
    gchar *id, *seat, *path;
    guint32 user_id;
    gchar *service;

    g_variant_get(result, "(a(susso))", &iter);

    while (g_variant_iter_loop(iter, "(susso)", &id, &user_id, &service, &seat, &path)) {
        if (g_strcmp0(seat, "seat0") == 0 && !session_id)
            session_id = g_strdup(id);
    }

    g_variant_iter_free(iter);

    return session_id;
}

static void
handle_wake_gesture(GestureSensors *app)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) result = NULL;

    result = g_dbus_connection_call_sync(app->dbus_connection,
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

    if (error) {
        g_warning("Failed to reset wake gesture: %s", error->message);
        g_clear_error(&error);
    }

    g_clear_pointer(&result, g_variant_unref);

    result = g_dbus_connection_call_sync(app->dbus_connection,
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

    if (error)
        g_warning("Failed to reset tilt detector: %s", error->message);

    g_clear_pointer(&result, g_variant_unref);

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
    g_autoptr(GVariant) changed_properties = NULL;
    g_autoptr(GVariant) invalidated_properties = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)",
                  &property_interface,
                  &changed_properties,
                  &invalidated_properties);

    g_autoptr(GVariant) idle_variant = g_variant_lookup_value(changed_properties, "IdleHint", G_VARIANT_TYPE_BOOLEAN);
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
    }
}

static void
subscribe_to_idle_hint(GestureSensors *app)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *session_path = NULL;

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
}

static void
on_palm_rejection_changed(GSettings *settings,
                          const gchar *key,
                          gpointer user_data)
{
    gboolean enabled = g_settings_get_boolean(settings, "palm-rejection-enabled");
    g_debug("Palm rejection %s", enabled ? "enabled" : "disabled");
    write_to_file(PALM_REJECTION_PATH, enabled ? "1" : "0");
}

static void
on_glove_mode_changed(GSettings *settings,
                      const gchar *key,
                       gpointer user_data)
{
    gboolean enabled = g_settings_get_boolean(settings, "glove-mode-enabled");
    g_debug("Glove mode %s", enabled ? "enabled" : "disabled");
    write_to_file(GLOVE_MODE_PATH, enabled ? "1" : "0");
}

static void
init_gsettings(GestureSensors *app)
{
    gboolean palm_supported = (access(PALM_REJECTION_PATH, F_OK) == 0);
    g_settings_set_boolean(app->settings, "palm-rejection-supported", palm_supported);
    g_debug("Palm rejection %s", palm_supported ? "is supported" : "is not supported");

    gboolean glove_supported = (access(GLOVE_MODE_PATH, F_OK) == 0);
    g_settings_set_boolean(app->settings, "glove-mode-supported", glove_supported);
    g_debug("Glove mode %s", glove_supported ? "is supported" : "is not supported");

    if (palm_supported) {
        gboolean palm_rejection = g_settings_get_boolean(app->settings, "palm-rejection-enabled");
        write_to_file(PALM_REJECTION_PATH, palm_rejection ? "1" : "0");
        g_signal_connect(app->settings, "changed::palm-rejection-enabled",
                         G_CALLBACK(on_palm_rejection_changed), NULL);
    }

    if (glove_supported) {
        gboolean glove_mode = g_settings_get_boolean(app->settings, "glove-mode-enabled");
        write_to_file(GLOVE_MODE_PATH, glove_mode ? "1" : "0");
        g_signal_connect(app->settings, "changed::glove-mode-enabled",
                         G_CALLBACK(on_glove_mode_changed), NULL);
    }
}

static void
cleanup_and_exit(GestureSensors *app)
{
    if (app->idle_source_id > 0) {
        g_source_remove(app->idle_source_id);
        app->idle_source_id = 0;
    }
    if (app->subscription_id > 0) {
        g_dbus_connection_signal_unsubscribe(app->dbus_connection, app->subscription_id);
        app->subscription_id = 0;
    }
    if (app->wake_session_id != -1) {
        release_wake_sensor(app, app->wake_session_id);
        app->wake_session_id = -1;
    }
    if (app->tilt_session_id != -1) {
        release_tilt_sensor(app, app->tilt_session_id);
        app->tilt_session_id = -1;
    }
    if (app->dbus_connection) {
        g_dbus_connection_flush_sync(app->dbus_connection, NULL, NULL);
        g_object_unref(app->dbus_connection);
        app->dbus_connection = NULL;
    }
    if (app->settings) {
        g_object_unref(app->settings);
        app->settings = NULL;
    }
    if (app->logind_session_id) {
        g_free(app->logind_session_id);
        app->logind_session_id = NULL;
    }
    if (app->main_loop) {
        g_main_loop_quit(app->main_loop);
        g_main_loop_unref(app->main_loop);
        app->main_loop = NULL;
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

    init_gsettings(&app);

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
