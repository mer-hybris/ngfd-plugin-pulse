/*
 * ngfd - Non-graphic feedback daemon
 *
 * Copyright (C) 2010 Nokia Corporation.
 * Contact: Xun Chen <xun.chen@nokia.com>
 *
 * Copyright (C) 2014 Jolla Ltd.
 * Contact: Thomas Perl <thomas.perl@jolla.com>
 *
 * This work is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <ngf/plugin.h>

#include <sndfile.h>
#include <pulse/pulseaudio.h>
#include <string.h>

#define PULSE_KEY "plugin.pulse.data"
#define LOG_CAT  "pulse: "

#define PLUGIN_DEBUG(fmt, ...) N_DEBUG (LOG_CAT fmt, ##__VA_ARGS__)
#define PLUGIN_WARNING(fmt, ...) N_WARNING (LOG_CAT fmt, ##__VA_ARGS__)

// XXX: Copied from canberra plugin
#define SOUND_FILENAME_KEY "canberra.filename"
#define SOUND_VOLUME_KEY "sound.volume"

// Maximum number of queued samples before silently dropping requests
#define MAX_QUEUED_SAMPLES 0

// Found through experimentation
#define MINIMUM_STREAM_SIZE 2048

N_PLUGIN_NAME        ("pulse")
N_PLUGIN_VERSION     ("0.91.0")
N_PLUGIN_DESCRIPTION ("Low-latency audio feedback via libsndfile and PulseAudio")

struct PulsePluginPriv {
    gboolean valid;
    pa_threaded_mainloop *mainloop;
    pa_context *context;
    pa_proplist *proplist;
    SNDFILE *upload_file;
    GHashTable *cached_files;
    int queued_samples;
};

typedef struct PulsePluginPriv PulsePluginPriv;

typedef void (*pulse_plugin_finished_cb) (void *user_data);

static void
context_state_callback (pa_context *context, void *user_data)
{
    PulsePluginPriv *priv = (PulsePluginPriv *)user_data;

    switch (pa_context_get_state (context)) {
        case PA_CONTEXT_READY:
            if (priv->cached_files) {
                g_hash_table_unref (priv->cached_files);
                priv->cached_files = NULL;
            }

            // Create new hash table for cached files
            priv->cached_files = g_hash_table_new_full (g_str_hash,
                    g_str_equal, g_free, g_free);

            priv->valid = TRUE;
            break;
        case PA_CONTEXT_FAILED:
            priv->valid = FALSE;
            PLUGIN_WARNING ("Connection failed");
            break;
        case PA_CONTEXT_TERMINATED:
            priv->valid = FALSE;
            PLUGIN_WARNING ("Connection terminated");
            break;
        default:
            break;
    }

    pa_threaded_mainloop_signal (priv->mainloop, 0);
}

static void
play_sample_callback (pa_context *context, uint32_t index, void *user_data)
{
    PulsePluginPriv *priv = (PulsePluginPriv *)user_data;

    priv->queued_samples--;

    if (index == PA_INVALID_INDEX) {
        PLUGIN_WARNING ("Failure playing sound!");
    } else {
        PLUGIN_DEBUG ("Sample playing completed, queued: %d", priv->queued_samples);
    }
}

static void
stream_state_callback (pa_stream *stream, void *user_data)
{
    PulsePluginPriv *priv = (PulsePluginPriv *)user_data;
    pa_threaded_mainloop_signal (priv->mainloop, 0);
}

static void
stream_write_callback (pa_stream *stream, size_t nbytes, void *user_data)
{
    PLUGIN_DEBUG ("stream_write_callback: %d bytes", nbytes);

    PulsePluginPriv *priv = (PulsePluginPriv *)user_data;

    if (!priv->upload_file) {
        PLUGIN_WARNING ("%s called without upload file", __func__);
        return;
    }

    char *data = g_malloc0(nbytes);
    sf_count_t count = sf_read_raw(priv->upload_file, data, nbytes);

    if (count != nbytes) {
        PLUGIN_WARNING ("Failed to read enough data from sf_read_raw()");
    }

    int ret = pa_stream_write (stream, data, count, NULL, 0, PA_SEEK_RELATIVE);
    if (ret < 0) {
        PLUGIN_WARNING ("Error writing to stream");
    }

    pa_stream_finish_upload (stream);

    g_free (data);
}

static void
pulse_plugin_priv_connect (PulsePluginPriv *priv)
{
    pa_mainloop_api *api = pa_threaded_mainloop_get_api (priv->mainloop);

    // Disconnect any possible old connection
    if (priv->context) {
        pa_context_disconnect (priv->context);
        pa_context_unref (priv->context);
        priv->context = NULL;
    }

    priv->context = pa_context_new (api, "ngfd-pulse");

    if (priv->context == NULL) {
        PLUGIN_WARNING ("Unable to create PulseAudio context");
        return;
    }

    pa_context_set_state_callback (priv->context, context_state_callback, priv);

    pa_context_connect (priv->context,
                        NULL /* default server */,
                        PA_CONTEXT_NOFLAGS,
                        0);

    // Wait for connection
    while (1) {
        pa_context_state_t state = pa_context_get_state (priv->context);

        PLUGIN_DEBUG ("Waiting for connection (%d)", state);

        if ((state == PA_CONTEXT_READY) ||
            (state == PA_CONTEXT_FAILED) ||
            (state == PA_CONTEXT_TERMINATED)) {
            break;
        }

        pa_threaded_mainloop_wait (priv->mainloop);
    }

    if (!priv->valid) {
        PLUGIN_WARNING ("Context is not valid");
    }
}

static gboolean
pulse_plugin_priv_cache_file (PulsePluginPriv *priv, const char *filename,
        const char *sound_id)
{
    PLUGIN_DEBUG ("Cache file proc: %s", filename);

    gboolean finished = FALSE;
    gboolean success = TRUE;
    gchar *id = g_strdup_printf ("ngfd_pulse_%s", sound_id);

    SF_INFO info;
    memset(&info, 0, sizeof(SF_INFO));
    SNDFILE *wav = sf_open(filename, SFM_READ, &info);

    if (!wav) {
        PLUGIN_WARNING ("Unable to open file: %s", filename);
        return FALSE;
    }

    int format = info.format & SF_FORMAT_TYPEMASK;
    int subtype = info.format & SF_FORMAT_SUBMASK;

    if (format != SF_FORMAT_WAV || subtype != SF_FORMAT_PCM_16) {
        PLUGIN_WARNING ("Supporting only PCM 16-bit wav files for now");
        sf_close (wav);
        return FALSE;
    }

    pa_sample_spec spec;
    spec.rate = info.samplerate;
    spec.channels = info.channels;
    spec.format = PA_SAMPLE_S16LE; // need support for big endian?

    PLUGIN_DEBUG ("Creating new stream, rate: %d, channels: %d", spec.rate, spec.channels);

    pa_stream *stream = pa_stream_new (priv->context, id, &spec, NULL /* default channel map */);

    if (!stream) {
        PLUGIN_WARNING ("Unable to create stream for caching");
        sf_close (wav);
        return FALSE;
    }

    pa_stream_set_state_callback (stream, stream_state_callback, priv);
    pa_stream_set_write_callback (stream, stream_write_callback, priv);

    size_t size = MAX(MINIMUM_STREAM_SIZE, info.frames * 2);

    PLUGIN_DEBUG ("Connecting to upload stream (%d frames)", size);

    int ret = pa_stream_connect_upload (stream, size);
    if (ret < 0) {
        PLUGIN_WARNING ("Failed to create upload stream");
        pa_stream_unref (stream);
        sf_close (wav);
        return FALSE;
    }

    priv->upload_file = wav;

    while (!finished) {
        switch (pa_stream_get_state (stream)) {
            case PA_STREAM_FAILED:
                PLUGIN_DEBUG ("Upload of stream failed");
                finished = TRUE;
                success = FALSE;
                break;
            case PA_STREAM_TERMINATED:
                PLUGIN_DEBUG ("Upload of stream successful");
                finished = TRUE;
                success = TRUE;
                g_hash_table_insert (priv->cached_files,
                        g_strdup (filename),
                        g_strdup (id));
                break;
            default:
                break;
        }

        if (finished) {
            break;
        }

        pa_threaded_mainloop_wait(priv->mainloop);
    }

    PLUGIN_DEBUG ("Upload done (success=%s)", success ? "true" : "false");

    priv->upload_file = NULL;

    pa_stream_unref (stream);
    sf_close (wav);
    g_free (id);

    return success;

}

static void
pulse_plugin_priv_play (PulsePluginPriv *priv, PulsePluginData *data,
        pulse_plugin_finished_cb finished_cb, void *user_data)
{
    gboolean can_play = TRUE;
    PLUGIN_DEBUG ("Would play: %s", data->filename);

    // XXX: XDG Sound Theme Spec
    gchar *fn = g_strdup_printf("/usr/share/sounds/jolla-ambient/stereo/%s.wav", data->filename);

    if (!g_file_test(fn, G_FILE_TEST_EXISTS)) {
        PLUGIN_WARNING ("File does not exist: %s", fn);
        goto cleanup;
    }

    pa_threaded_mainloop_lock(priv->mainloop);

    if (!priv->valid) {
        // Reconnect if connection is invalid
        pulse_plugin_priv_connect (priv);
    }

    if (!g_hash_table_lookup (priv->cached_files, fn)) {
        PLUGIN_DEBUG ("Need to cache file: %s", fn);
        if (!pulse_plugin_priv_cache_file (priv, fn, data->filename)) {
            can_play = FALSE;
        }
    }

    if (!priv->valid) {
        PLUGIN_WARNING ("Cannot playback file: Invalid PulseAudio context");
    } else if (!can_play) {
        PLUGIN_WARNING ("Cannot playback file: Caching failed");
    } else if (priv->queued_samples > MAX_QUEUED_SAMPLES) {
        PLUGIN_DEBUG ("Skipping playback: Playback queue is full");
    } else {
        PLUGIN_DEBUG ("Playing sound effect: %s", fn);

        pa_operation *op = pa_context_play_sample_with_proplist(priv->context,
                g_hash_table_lookup (priv->cached_files, fn),
                NULL /* default sink */,
                -1 /* server decides volume */,
                priv->proplist,
                play_sample_callback, priv);

        if (op) {
            priv->queued_samples++;
            pa_operation_unref (op);
        } else {
            PLUGIN_WARNING ("Playback of cached sample failed");
        }
    }

    pa_threaded_mainloop_unlock(priv->mainloop);

cleanup:
    g_free(fn);

    if (finished_cb) {
        finished_cb (user_data);
    }
}

struct PulsePluginData {
    NRequest *request;
    NSinkInterface *iface;
    gchar *filename;
    gboolean sound_enabled;
};

typedef struct PulsePluginData PulsePluginData;


static PulsePluginPriv *
priv = NULL;

static int
pulse_sink_initialize (NSinkInterface *iface)
{
    (void) iface;
    PLUGIN_DEBUG ("sink initialize");

    g_assert (priv == NULL);
    priv = g_slice_new0 (PulsePluginPriv);

    priv->valid = FALSE;
    priv->mainloop = pa_threaded_mainloop_new ();

    if (priv->mainloop == NULL) {
        PLUGIN_WARNING ("Failed to initialize PulseAudio mainloop");
        g_slice_free (PulsePluginPriv, priv);
        priv = NULL;
        return FALSE;
    }

    // follow feedback sound level and mute
    priv->proplist = pa_proplist_new ();
    // XXX: Take from sound.stream.* properties
    pa_proplist_sets (priv->proplist,
            "module-stream-restore.id",
            "x-meego-feedback-sound-level");
    pa_proplist_sets (priv->proplist,
            "media.name",
            "feedback-event");

    pa_threaded_mainloop_lock (priv->mainloop);
    pa_threaded_mainloop_start (priv->mainloop);

    pulse_plugin_priv_connect (priv);

    pa_threaded_mainloop_unlock (priv->mainloop);

    return TRUE;
}

static void
pulse_sink_shutdown (NSinkInterface *iface)
{
    (void) iface;

    PLUGIN_DEBUG ("sink shutdown");

    if (priv) {
        // TODO: pa_context_remove_sample for all cached samples

        if (priv->cached_files) {
            g_hash_table_unref (priv->cached_files);
        }

        if (priv->proplist) {
            pa_proplist_free (priv->proplist);
        }

        if (priv->context) {
            pa_context_disconnect (priv->context);
            pa_context_unref (priv->context);
        }

        if (priv->mainloop) {
            pa_threaded_mainloop_stop (priv->mainloop);
            pa_threaded_mainloop_free (priv->mainloop);
        }

        g_slice_free (PulsePluginPriv, priv);
        priv = NULL;
    }
}

static int
pulse_sink_can_handle (NSinkInterface *iface, NRequest *request)
{
    (void) iface;

    PLUGIN_DEBUG ("sink can_handle");

    NProplist *props = NULL;

    props = (NProplist*) n_request_get_properties (request);
    if (n_proplist_has_key (props, SOUND_FILENAME_KEY)) {
        PLUGIN_DEBUG ("Request has %s, we can handle this.", SOUND_FILENAME_KEY);
        return TRUE;
    }

    return FALSE;
}

static int
pulse_sink_prepare (NSinkInterface *iface, NRequest *request)
{
    PLUGIN_DEBUG ("sink prepare");

    PulsePluginData *data = g_slice_new0 (PulsePluginData);

    NProplist *props = props = (NProplist*) n_request_get_properties (request);

    data->request = request;
    data->iface = iface;
    data->filename = g_strdup (n_proplist_get_string (props, SOUND_FILENAME_KEY));

    if (n_proplist_has_key (props, SOUND_VOLUME_KEY)) {
        data->sound_enabled = n_proplist_get_int (props, SOUND_VOLUME_KEY) > 0;
    } else {
        data->sound_enabled = TRUE;
    }

    n_request_store_data (request, PULSE_KEY, data);
    n_sink_interface_synchronize (iface, request);

    return TRUE;
}

static void
finished_callback (gpointer userdata)
{
    PLUGIN_DEBUG ("sink finished");

    PulsePluginData *data = (PulsePluginData*) userdata;
    g_assert (data != NULL);

    n_sink_interface_complete (data->iface, data->request);
}

static int
pulse_sink_play (NSinkInterface *iface, NRequest *request)
{
    PLUGIN_DEBUG ("sink play");

    (void) iface;

    PulsePluginData *data = (PulsePluginData*) n_request_get_data (request, PULSE_KEY);
    g_assert (data != NULL);

    pulse_plugin_priv_play (priv, data, finished_callback, data);

    return TRUE;
}

static int
pulse_sink_pause (NSinkInterface *iface, NRequest *request)
{
    PLUGIN_DEBUG ("sink pause");

    (void) iface;
    (void) request;

    // TODO: Pause current playbacks if possible

    return TRUE;
}

static void
pulse_sink_stop (NSinkInterface *iface, NRequest *request)
{
    PLUGIN_DEBUG ("sink stop");

    (void) iface;

    PulsePluginData *data = (PulsePluginData*) n_request_get_data (request, PULSE_KEY);
    g_assert (data != NULL);

    // TODO: Abort current playbacks if possible

    if (data->filename) {
        g_free (data->filename);
    }

    g_slice_free (PulsePluginData, data);
}

N_PLUGIN_LOAD (plugin)
{
    PLUGIN_DEBUG ("plugin load");

    static const NSinkInterfaceDecl decl = {
        .name       = "pulse",
        .initialize = pulse_sink_initialize,
        .shutdown   = pulse_sink_shutdown,
        .can_handle = pulse_sink_can_handle,
        .prepare    = pulse_sink_prepare,
        .play       = pulse_sink_play,
        .pause      = pulse_sink_pause,
        .stop       = pulse_sink_stop
    };

    n_plugin_register_sink (plugin, &decl);

    return TRUE;
}

N_PLUGIN_UNLOAD (plugin)
{
    (void) plugin;

    PLUGIN_DEBUG ("plugin unload");
}
