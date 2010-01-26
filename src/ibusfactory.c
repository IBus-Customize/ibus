/* vim:set et sts=4: */
/* ibus - The Input Bus
 * Copyright (C) 2008-2010 Peng Huang <shawn.p.huang@gmail.com>
 * Copyright (C) 2008-2010 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "ibusfactory.h"
#include "ibusengine.h"
#include "ibusshare.h"
#include "ibusinternal.h"

#define IBUS_FACTORY_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), IBUS_TYPE_FACTORY, IBusFactoryPrivate))

enum {
    LAST_SIGNAL,
};

/* IBusFactoryPriv */
struct _IBusFactoryPrivate {
    guint id;
    IBusConnection *connection;
    GList          *engine_list;
    GHashTable     *engine_table;
};
typedef struct _IBusFactoryPrivate IBusFactoryPrivate;

/* functions prototype */
static void     ibus_factory_class_init     (IBusFactoryClass   *klass);
static void     ibus_factory_init           (IBusFactory        *factory);
static void     ibus_factory_destroy        (IBusFactory        *factory);
static gboolean ibus_factory_ibus_message   (IBusFactory        *factory,
                                             IBusConnection     *connection,
                                             IBusMessage        *message);

static void     _engine_destroy_cb          (IBusEngine         *engine,
                                             IBusFactory        *factory);

static IBusServiceClass *factory_parent_class = NULL;

GType
ibus_factory_get_type (void)
{
    static GType type = 0;

    static const GTypeInfo type_info = {
        sizeof (IBusFactoryClass),
        (GBaseInitFunc)     NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc)    ibus_factory_class_init,
        NULL,               /* class finalize */
        NULL,               /* class data */
        sizeof (IBusFactory),
        0,
        (GInstanceInitFunc) ibus_factory_init,
    };

    if (type == 0) {
        type = g_type_register_static (IBUS_TYPE_SERVICE,
                    "IBusFactory",
                    &type_info,
                    (GTypeFlags) 0);
    }
    return type;
}

IBusFactory *
ibus_factory_new (IBusConnection *connection)
{
    g_assert (IBUS_IS_CONNECTION (connection));

    IBusFactory *factory;
    IBusFactoryPrivate *priv;

    factory = (IBusFactory *) g_object_new (IBUS_TYPE_FACTORY,
                                            "path", IBUS_PATH_FACTORY,
                                            NULL);
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    priv->connection = g_object_ref_sink (connection);
    ibus_service_add_to_connection ((IBusService *)factory, connection);

    return factory;
}

static void
ibus_factory_class_init (IBusFactoryClass *klass)
{
    // GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    IBusObjectClass *ibus_object_class = IBUS_OBJECT_CLASS (klass);

    factory_parent_class = (IBusServiceClass *) g_type_class_peek_parent (klass);

    g_type_class_add_private (klass, sizeof (IBusFactoryPrivate));

    ibus_object_class->destroy = (IBusObjectDestroyFunc) ibus_factory_destroy;

    IBUS_SERVICE_CLASS (klass)->ibus_message = (ServiceIBusMessageFunc) ibus_factory_ibus_message;

}

static void
ibus_factory_init (IBusFactory *factory)
{
    IBusFactoryPrivate *priv;
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    priv->id = 0;
    priv->connection = NULL;
    priv->engine_table = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                g_free,
                                                NULL);
    priv->engine_list =  NULL;
}

static void
ibus_factory_destroy (IBusFactory *factory)
{
    GList *list;
    IBusFactoryPrivate *priv;
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    list = g_list_copy (priv->engine_list);
    g_list_foreach (list, (GFunc) ibus_object_destroy, NULL);
    g_list_free (priv->engine_list);
    g_list_free (list);
    priv->engine_list = NULL;

    if (priv->engine_table) {
        g_hash_table_destroy (priv->engine_table);
    }

    if (priv->connection) {
        ibus_service_remove_from_connection ((IBusService *)factory,
                                             priv->connection);
        g_object_unref (priv->connection);
    }

    IBUS_OBJECT_CLASS(factory_parent_class)->destroy (IBUS_OBJECT (factory));
}

static void
_engine_destroy_cb (IBusEngine  *engine,
                    IBusFactory *factory)
{
    IBusFactoryPrivate *priv;
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    priv->engine_list = g_list_remove (priv->engine_list, engine);
    g_object_unref (engine);
}

static gboolean
ibus_factory_ibus_message (IBusFactory    *factory,
                           IBusConnection *connection,
                           IBusMessage    *message)
{
    g_assert (IBUS_IS_FACTORY (factory));
    g_assert (IBUS_IS_CONNECTION (connection));
    g_assert (message != NULL);

    IBusMessage *reply_message;
    IBusFactoryPrivate *priv;
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    g_assert (priv->connection == connection);

    if (ibus_message_is_method_call (message,
                                     IBUS_INTERFACE_FACTORY,
                                     "CreateEngine")) {
        gchar *engine_name;
        gchar *path;
        IBusError *error;
        IBusEngine *engine;
        gboolean retval;
        GType engine_type;

        retval = ibus_message_get_args (message,
                                        &error,
                                        G_TYPE_STRING, &engine_name,
                                        G_TYPE_INVALID);

        if (!retval) {
            reply_message = ibus_message_new_error_printf (message,
                                        DBUS_ERROR_INVALID_ARGS,
                                        "The 1st arg should be engine name");
            ibus_connection_send (connection, reply_message);
            ibus_message_unref (reply_message);
            return TRUE;
        }

        engine_type = (GType )g_hash_table_lookup (priv->engine_table, engine_name);

        if (engine_type == G_TYPE_INVALID) {
             reply_message = ibus_message_new_error_printf (message,
                                        DBUS_ERROR_FAILED,
                                        "Can not create engine %s", engine_name);
            ibus_connection_send (connection, reply_message);
            ibus_message_unref (reply_message);
            return TRUE;

        }

        path = g_strdup_printf ("/org/freedesktop/IBus/Engine/%d", ++priv->id);

        engine = g_object_new (engine_type,
                               "name", engine_name,
                               "path", path,
                               "connection", priv->connection,
                               NULL);

        priv->engine_list = g_list_append (priv->engine_list, engine);
        g_signal_connect (engine,
                          "destroy",
                          G_CALLBACK (_engine_destroy_cb),
                          factory);

        reply_message = ibus_message_new_method_return (message);
        ibus_message_append_args (reply_message,
                                  IBUS_TYPE_OBJECT_PATH, &path,
                                  G_TYPE_INVALID);
        g_free (path);
        ibus_connection_send (connection, reply_message);
        ibus_message_unref (reply_message);
        return TRUE;
    }

    return factory_parent_class->ibus_message ((IBusService *)factory,
                                               connection,
                                               message);
}

void
ibus_factory_add_engine (IBusFactory *factory,
                         const gchar *engine_name,
                         GType        engine_type)
{
    g_assert (IBUS_IS_FACTORY (factory));
    g_assert (engine_name);
    g_assert (g_type_is_a (engine_type, IBUS_TYPE_ENGINE));

    IBusFactoryPrivate *priv;
    priv = IBUS_FACTORY_GET_PRIVATE (factory);

    g_hash_table_insert (priv->engine_table, g_strdup (engine_name), (gpointer) engine_type);
}
