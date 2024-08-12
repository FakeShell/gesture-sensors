#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <inttypes.h>
#include <batman/wlrdisplay.h>
#include "virtkey.h"

volatile sig_atomic_t g_quit = 0;

typedef struct {
    GDBusConnection *dbus_connection;
    gint32 session_id;
    GMainLoop *main_loop;
    gboolean previous_screen_on;
} WakeGestureApp;

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
        g_print("Unknown key 'Escape'");
        return;
    }
    cmd->key_codes = malloc(sizeof(cmd->key_codes[0]));
    cmd->key_codes_len = 1;
    cmd->key_codes[0] = get_key_code_by_xkb(&wtype, ks);
    cmd->delay_ms = 0;

    wtype.display = wl_display_connect(NULL);
    if (wtype.display == NULL) {
        g_print("Wayland connection failed\n");
        return;
    }
    wtype.registry = wl_display_get_registry(wtype.display);
    wl_registry_add_listener(wtype.registry, &registry_listener, &wtype);
    wl_display_dispatch(wtype.display);
    wl_display_roundtrip(wtype.display);

    if (wtype.manager == NULL) {
        g_print("Compositor does not support the virtual keyboard protocol\n");
        return;
    }
    if (wtype.seat == NULL) {
        g_print("No seat found\n");
        return;
    }

    wtype.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        wtype.manager, wtype.seat
    );

    upload_keymap(&wtype);
    run_commands(&wtype);

    g_print("Tab key sent to seat\n");

    free(wtype.commands);
    free(wtype.keymap);
    zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
    zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
    wl_registry_destroy(wtype.registry);
    wl_display_disconnect(wtype.display);
}

gint32
request_sensor(WakeGestureApp *app)
{
    GVariant *result;
    GError *error = NULL;
    gboolean standby_success = FALSE;
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
release_sensor(WakeGestureApp *app, gint32 session_id)
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
get_sensor_reading(WakeGestureApp *app)
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

static gboolean
poll_sensor(gpointer user_data)
{
    WakeGestureApp *app = (WakeGestureApp *)user_data;

    int result = wlrdisplay(0, NULL);
    gboolean current_screen_on = (result == 0);

    if (current_screen_on) {
        printf("screen is on, skip for now\n");
        app->previous_screen_on = TRUE;
        g_usleep(2000000);
        return G_SOURCE_CONTINUE;
    }

    // if screen has been on for a while, then we won't be polling sensorfw for a while and it will drop the session id
    // request a session id again when previous state was screen on and now its time to use the wake gesture again
    if (app->previous_screen_on && !current_screen_on) {
        printf("Screen turned off, releasing and requesting sensor\n");
        release_sensor(app, app->session_id);
        app->session_id = request_sensor(app);
        if (app->session_id == -1) {
            g_printerr("Failed to request new sensor after screen state change\n");
            g_main_loop_quit(app->main_loop);
            return G_SOURCE_REMOVE;
        }
    }

    app->previous_screen_on = current_screen_on;

    guint32 reading = get_sensor_reading(app);
    if (reading == 1) {
        printf("Wake gesture detected! Reading: %u\n", reading);

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
        if (error) {
            g_warning("Failed to stop sensor: %s", error->message);
            g_error_free(error);
        }

        release_sensor(app, app->session_id);
        app->session_id = request_sensor(app);
        if (app->session_id == -1) {
            g_printerr("Failed to request new sensor after reset\n");
            g_main_loop_quit(app->main_loop);
            return G_SOURCE_REMOVE;
        }

        send_wake_key();
    }

    return G_SOURCE_CONTINUE;
}

static void
cleanup_and_exit(WakeGestureApp *app)
{
    if (app->session_id != -1) {
        release_sensor(app, app->session_id);
    }

    if (app->dbus_connection) {
        g_object_unref(app->dbus_connection);
    }

    if (app->main_loop) {
        g_main_loop_quit(app->main_loop);
        g_main_loop_unref(app->main_loop);
    }
}

static void
signal_handler(int signum)
{
    printf("Caught signal %d, exiting...\n", signum);
    g_quit = 1;
}

static gboolean
check_quit_flag(gpointer user_data)
{
    WakeGestureApp *app = (WakeGestureApp *)user_data;
    if (g_quit) {
        g_main_loop_quit(app->main_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
    WakeGestureApp app = {0};
    GError *error = NULL;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    app.dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app.dbus_connection) {
        g_printerr("Failed to connect to D-Bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    app.session_id = request_sensor(&app);
    if (app.session_id == -1) {
        g_printerr("Failed to request sensor\n");
        cleanup_and_exit(&app);
        return 1;
    }

    app.main_loop = g_main_loop_new(NULL, FALSE);

    g_timeout_add(500, poll_sensor, &app);
    g_timeout_add(500, check_quit_flag, &app);

    g_main_loop_run(app.main_loop);

    cleanup_and_exit(&app);

    return 0;
}
