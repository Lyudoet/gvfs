/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <libsoup/soup.h>

/* LibXML2 includes */
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "gvfsbackenddav.h"
#include "gvfsjobmount.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

#include "soup-input-stream.h"
#include "soup-output-stream.h"

typedef struct _MountAuthInfo MountAuthInfo;

static void mount_auth_info_free (MountAuthInfo *info);

struct _MountAuthInfo {

  SoupSession  *session;
  GMountSource *mount_source;

     /* for server authentication */
  char *username;
  char *password;
  char *last_realm;

     /* for proxy authentication */
  char *proxy_user;
  char *proxy_password;

};



struct _GVfsBackendDav
{
  GVfsBackendHttp parent_instance;

  MountAuthInfo auth_info;
};

G_DEFINE_TYPE (GVfsBackendDav, g_vfs_backend_dav, G_VFS_TYPE_BACKEND_HTTP)

static void
g_vfs_backend_dav_finalize (GObject *object)
{
  GVfsBackendDav *dav_backend;

  dav_backend = G_VFS_BACKEND_DAV (object);

  mount_auth_info_free (&(dav_backend->auth_info));
  
  if (G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_dav_parent_class)->finalize) (object);
}

static void
g_vfs_backend_dav_init (GVfsBackendDav *backend)
{
}

/* ************************************************************************* */
/*  */

static inline gboolean
sm_has_header (SoupMessage *msg, const char *header)
{
  return soup_message_headers_get (msg->response_headers, header) != NULL;
}

static inline void
send_message (GVfsBackend         *backend,
              SoupMessage         *message, 
              SoupSessionCallback  callback,
              gpointer             user_data)
{

  soup_session_queue_message (G_VFS_BACKEND_HTTP (backend)->session,
                              message,
                              callback, user_data);
}

static char *
path_get_parent_dir (const char *path)
{
  char   *parent;
  size_t  len;

  if ((len = strlen (path)) < 1)
    return NULL;

  /* maybe this should be while, but then again
   * I should be reading the uri rfc and see 
   * what the deal was with multiple slashes */

  if (path[len - 1] == '/')
    len--;

  parent = g_strrstr_len (path, len, "/");

  if (parent == NULL)
    return NULL;

  return g_strndup (path, (parent - path) + 1);
}

/* ************************************************************************* */
/* generic xml parsing functions */

static inline gboolean
node_has_name (xmlNodePtr node, const char *name)
{
  g_return_val_if_fail (node != NULL, FALSE);

  return ! strcmp ((char *) node->name, name);
}

static inline gboolean
node_has_name_ns (xmlNodePtr node, const char *name, const char *ns_href)
{
  gboolean has_name;
  gboolean has_ns;

  g_return_val_if_fail (node != NULL, FALSE);

  has_name = has_ns = TRUE;

  if (name)
    has_name = node->name && ! strcmp ((char *) node->name, name);

  if (ns_href)
    has_ns = node->ns && node->ns->href &&
      ! g_ascii_strcasecmp ((char *) node->ns->href, ns_href);

  return has_name && has_ns;
}

static inline gboolean
node_is_element (xmlNodePtr node)
{
  return node->type == XML_ELEMENT_NODE && node->name != NULL;
}


static inline gboolean
node_is_element_with_name (xmlNodePtr node, const char *name)
{
  return node->type == XML_ELEMENT_NODE &&
    node->name != NULL && 
    ! strcmp ((char *) node->name, name);
}

static const char *
node_get_content (xmlNodePtr node)
{
    if (node == NULL)
      return NULL;

    switch (node->type)
      {
        case XML_ELEMENT_NODE:
          return node_get_content (node->children);
          break;
        case XML_TEXT_NODE:
          return (const char *) node->content;
          break;
        default:
          return NULL;
      }
}

typedef struct _xmlNodeIter {

  xmlNodePtr cur_node;
  xmlNodePtr next_node;

  const char *name;
  const char *ns_href;

  void       *user_data;

} xmlNodeIter;

static xmlNodePtr
xml_node_iter_next (xmlNodeIter *iter)
{
  xmlNodePtr node;

  while ((node = iter->next_node))
    {
      iter->next_node = node->next;

      if (node->type == XML_ELEMENT_NODE) {
        if (node_has_name_ns (node, iter->name, iter->ns_href))
          break;
      }
    }

  iter->cur_node = node;
  return node;
}

static void *
xml_node_iter_get_user_data (xmlNodeIter *iter)
{
  return iter->user_data;
}

static xmlNodePtr
xml_node_iter_get_current (xmlNodeIter *iter)
{
  return iter->cur_node;
}

static xmlDocPtr
parse_xml (SoupMessage  *msg,
           xmlNodePtr   *root,
           const char   *name,
           GError      **error)
{
 xmlDocPtr  doc;

  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("HTTP Error: %s"), msg->reason_phrase);
      return NULL;
    }

  doc = xmlReadMemory (msg->response_body->data,
                       msg->response_body->length,
                       "response.xml",
                       NULL,
                       XML_PARSE_NOWARNING |
                       XML_PARSE_NOBLANKS |
                       XML_PARSE_NSCLEAN |
                       XML_PARSE_NOCDATA |
                       XML_PARSE_COMPACT);
  if (doc == NULL)
    { 
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", _("Could not parse response"));
      return NULL;
    }

  *root = xmlDocGetRootElement (doc);

  if (*root == NULL || (*root)->children == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", _("Empty response"));
      return NULL;
    }

  if (strcmp ((char *) (*root)->name, name))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "%s", _("Unexpected reply from server"));
      return NULL;
    }

  return doc;
}

/* ************************************************************************* */
/* Multistatus parsing code */

typedef struct _Multistatus Multistatus;
typedef struct _MsResponse MsResponse;
typedef struct _MsPropstat MsPropstat;

struct _Multistatus {

  xmlDocPtr  doc;
  xmlNodePtr root;

  const SoupURI *target;

};

struct _MsResponse {

  Multistatus *multistatus;

  xmlNodePtr  href;
  xmlNodePtr  first_propstat;
};

struct _MsPropstat {

  Multistatus *multistatus;

  xmlNodePtr   prop_node;
  guint        status_code;

};


static gboolean
multistatus_parse (SoupMessage *msg, Multistatus *multistatus, GError **error)
{
  xmlDocPtr  doc;
  xmlNodePtr root;

  doc = parse_xml (msg, &root, "multistatus", error);

  if (doc == NULL)
    return FALSE;

  multistatus->doc = doc;
  multistatus->root = root;
  multistatus->target = soup_message_get_uri (msg);

  return TRUE;
}

static void
multistatus_free (Multistatus *multistatus)
{
  xmlFreeDoc (multistatus->doc);
}

static void
multistatus_get_response_iter (Multistatus *multistatus, xmlNodeIter *iter)
{
  iter->cur_node = multistatus->root->children;
  iter->next_node = multistatus->root->children;
  iter->name = "response";
  iter->ns_href = "DAV:";
  iter->user_data = multistatus;
}

static gboolean
multistatus_get_response (xmlNodeIter *resp_iter, MsResponse *response)
{
  Multistatus *multistatus;
  xmlNodePtr   resp_node;
  xmlNodePtr   iter;
  xmlNodePtr   href;
  xmlNodePtr   propstat;

  multistatus = xml_node_iter_get_user_data (resp_iter);
  resp_node = xml_node_iter_get_current (resp_iter);

  if (resp_node == NULL)
    return FALSE;

  propstat = NULL;
  href = NULL;

  for (iter = resp_node->children; iter; iter = iter->next)
    {
      if (! node_is_element (iter))
        {
          continue;
        }
      else if (node_has_name_ns (iter, "href", "DAV:"))
        {
          href = iter;
        }
      else if (node_has_name_ns (iter, "propstat", "DAV:"))
        {
          if (propstat == NULL)
            propstat = iter;
        }

      if (href && propstat)
        break;
    }

  if (href == NULL)
    return FALSE;

  response->href = href;
  response->multistatus = multistatus;
  response->first_propstat = propstat;

  return resp_node != NULL;
}

static char *
ms_response_get_basename (MsResponse *response)
{
  const char *text;
  text = node_get_content (response->href);

  return uri_get_basename (text);

}

static gboolean
ms_response_is_target (MsResponse *response)
{
  const char    *text;
  const char    *path;
  const SoupURI *target;
  SoupURI       *uri;
  gboolean       res;

  res    = FALSE;
  uri    = NULL;
  path   = NULL;
  target = response->multistatus->target;
  text   = node_get_content (response->href);

  if (text == NULL)
    return FALSE;

  if (*text == '/')
    {
      path = text;
    }
  else if (!g_ascii_strncasecmp (text, "http", 4))
    {
      uri = soup_uri_new (text);
      path = uri->path;
    }

  if (path)
    {
      size_t len_path, len_target;

      len_path = strlen (path);
      len_target = strlen (target->path);

      while (path[len_path - 1] == '/')
        len_path--;

      while (path[len_target - 1] == '/')
        len_target--;

      if (len_path == len_target)
        res = ! g_ascii_strncasecmp (path, target->path, len_path);
    }

  if (uri)
    soup_uri_free (uri);
  
  return res;
}

static void
ms_response_get_propstat_iter (MsResponse *response, xmlNodeIter *iter)
{
  iter->cur_node = response->first_propstat;
  iter->next_node = response->first_propstat;
  iter->name = "propstat";
  iter->ns_href = "DAV:"; 
  iter->user_data = response;
}

static guint
ms_response_get_propstat (xmlNodeIter *cur_node, MsPropstat *propstat)
{
  MsResponse *response;
  xmlNodePtr  pstat_node;
  xmlNodePtr  iter;
  xmlNodePtr  prop;
  xmlNodePtr  status;
  const char *status_text;
  gboolean    res;
  guint       code;

  response = xml_node_iter_get_user_data (cur_node);
  pstat_node = xml_node_iter_get_current (cur_node);

  if (pstat_node == NULL)
    return 0;

  status = NULL;
  prop = NULL;

  for (iter = pstat_node->children; iter; iter = iter->next)
    {
      if (!node_is_element (iter))
        {
          continue;
        }
      else if (node_has_name_ns (iter, "status", "DAV:"))
        {
          status = iter;
        }
      else if (node_has_name_ns (iter, "prop", "DAV:"))
        {
          prop = iter;
        }

      if (status && prop)
        break;
    }

  status_text = node_get_content (status);

  if (status_text == NULL || prop == NULL)
    return 0;

  res = soup_headers_parse_status_line ((char *) status_text,
                                        NULL,
                                        &code,
                                        NULL);

  if (res == FALSE)
    return 0;

  propstat->prop_node = prop;
  propstat->status_code = code;
  propstat->multistatus = response->multistatus;

  return code;
}

static GFileType
parse_resourcetype (xmlNodePtr rt)
{
  xmlNodePtr node;
  GFileType  type;

  for (node = rt->children; node; node = node->next)
    { 
      if (node_is_element (node))
          break;
    }

  if (node == NULL)
    return G_FILE_TYPE_REGULAR;

  if (! strcmp ((char *) node->name, "collection"))
    type = G_FILE_TYPE_DIRECTORY;
  else if (! strcmp ((char *) node->name, "redirectref"))
    type = G_FILE_TYPE_SYMBOLIC_LINK;
  else
    type = G_FILE_TYPE_UNKNOWN;

  return type;
}

static void
ms_response_to_file_info (MsResponse *response,
                          GFileInfo  *info)
{
  xmlNodeIter iter;
  MsPropstat  propstat;
  xmlNodePtr  node;
  guint       status;
  char       *basename;
  const char *text;
  GTimeVal    tv;

  basename = ms_response_get_basename (response);
  g_file_info_set_name (info, basename);
  g_file_info_set_edit_name (info, basename);
  g_free (basename);

  ms_response_get_propstat_iter (response, &iter);
  while (xml_node_iter_next (&iter))
    {
      status = ms_response_get_propstat (&iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (node = propstat.prop_node->children; node; node = node->next)
        {
          if (! node_is_element (node))
            continue; /* FIXME: check namespace, parse user data nodes*/

          text = node_get_content (node);

          if (node_has_name (node, "resourcetype"))
            {
              GFileType type = parse_resourcetype (node);
              g_file_info_set_file_type (info, type);
            }
          else if (node_has_name (node, "displayname"))
            {
              g_file_info_set_display_name (info, text);
            }
          else if (node_has_name (node, "getetag"))
            {
              g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE,
                                                text);
            }
          else if (node_has_name (node, "creationdate"))
            {
              if (! g_time_val_from_iso8601 (text, &tv))
                continue;

              g_file_info_set_attribute_uint64 (info,
                                                G_FILE_ATTRIBUTE_TIME_CREATED,
                                                tv.tv_sec);
            }
          else if (node_has_name (node, "getcontenttype"))
            {
              g_file_info_set_content_type (info, text);
            }
          else if (node_has_name (node, "getcontentlength"))
            {
              gint64 size;
              size = g_ascii_strtoll (text, NULL, 10);
              g_file_info_set_size (info, size);
            }
          else if (node_has_name (node, "getlastmodified"))
            {
              if (g_time_val_from_iso8601 (text, &tv))
                g_file_info_set_modification_time (info, &tv);
            }
        }
    }
}

static GFileType
ms_response_to_file_type (MsResponse *response)
{
  xmlNodeIter prop_iter;
  MsPropstat  propstat;
  GFileType   file_type;
  guint       status;

  file_type = G_FILE_TYPE_UNKNOWN;

  ms_response_get_propstat_iter (response, &prop_iter);
  while (xml_node_iter_next (&prop_iter))
    {
      xmlNodePtr iter;

      status = ms_response_get_propstat (&prop_iter, &propstat);

      if (! SOUP_STATUS_IS_SUCCESSFUL (status))
        continue;

      for (iter = propstat.prop_node->children; iter; iter = iter->next)
        {
          if (node_is_element (iter) &&
              node_has_name_ns (iter, "resourcetype", "DAV:"))
            break;
        }

      if (iter)
        {
          file_type = parse_resourcetype (iter);
          break;
        }
    }

  return file_type;
}

#define PROPSTAT_XML_BEGIN                        \
  "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n" \
  " <D:propfind xmlns:D=\"DAV:\">\n"

#define PROPSTAT_XML_ALLPROP "  <D:allprop/>\n"
#define PROPSTAT_XML_PROP_BEGIN "  <D:prop>\n"
#define PROPSTAT_XML_PROP_END   "  </D:prop>\n"

#define PROPSTAT_XML_END                          \
  " </D:propfind>"

typedef struct _PropName {
  
  const char *name;
  const char *namespace;

} PropName;

static SoupMessage *
propfind_request_new (GVfsBackend     *backend,
                      const char      *filename,
                      guint            depth,
                      const PropName  *properties)
{
  SoupMessage *msg;
  const char  *header_depth;
  GString     *body;

  msg = message_new_from_filename_full (backend, SOUP_METHOD_PROPFIND,
                                        filename, (depth > 0));

  if (msg == NULL)
    return NULL;

  if (depth == 0)
    header_depth = "0";
  else if (depth == 1)
    header_depth = "1";
  else
    header_depth = "infinity";

  soup_message_headers_append (msg->request_headers, "Depth", header_depth);

  body = g_string_new (PROPSTAT_XML_BEGIN);

  if (properties != NULL)
    {
      const PropName *prop;

      for (prop = properties; prop->name; prop++)
        {
          if (prop->namespace != NULL)
            g_string_append (body, "<%s xmlns=\"%s\"/>");
          else
            g_string_append (body, "<D:%s/>");
        }
    }
  else
    g_string_append (body, PROPSTAT_XML_ALLPROP);
    

  g_string_append (body, PROPSTAT_XML_END);

  soup_message_set_request (msg, "application/xml",
                            SOUP_MEMORY_TAKE,
                            body->str,
                            body->len);

  g_string_free (body, FALSE);

  return msg;
}

static void
message_add_apply_to_redirect_header (SoupMessage         *msg,
                                      GFileQueryInfoFlags  flags)
{
  const char  *header_redirect;

  /* RFC 4437 */
  if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
      header_redirect = "F";
  else
      header_redirect = "T";

  soup_message_headers_append (msg->request_headers,
                               "Apply-To-Redirect-Ref", header_redirect);
}
/* ************************************************************************* */
/*  */



static void
mount_auth_info_free (MountAuthInfo *data)
{
  if (data->mount_source)
    g_object_unref (data->mount_source);
  
  g_free (data->username);
  g_free (data->password);
  g_free (data->last_realm);
  g_free (data->proxy_user);
  g_free (data->proxy_password);

}

static void
soup_authenticate_from_data (SoupSession *session,
                             SoupMessage *msg,
                             SoupAuth    *auth,
                             gboolean     retrying,
                             gpointer     user_data)
{
  MountAuthInfo  *data;

  g_print ("+ soup_authenticate_from_data (%s) \n",
           retrying ? "retrying" : "first auth");

  data = (MountAuthInfo *) user_data;

  if (retrying)
    return;

  if (soup_auth_is_for_proxy (auth))
    soup_auth_authenticate (auth, data->proxy_user, data->proxy_password);
  else
    soup_auth_authenticate (auth, data->username, data->password);

}

static void
soup_authenticate_interactive (SoupSession *session,
                               SoupMessage *msg,
                               SoupAuth    *auth,
                               gboolean     retrying,
                               gpointer     user_data)
{
  MountAuthInfo  *data;
  const char     *username;
  const char     *password;
  gboolean        res;
  gboolean        aborted;
  char           *new_username;
  char           *new_password;
  char           *prompt;

  g_print ("+ soup_authenticate_interactive (%s) \n",
           retrying ? "retrying" : "first auth");

  data = (MountAuthInfo *) user_data;

  new_username = NULL;
  new_password = NULL;

  if (soup_auth_is_for_proxy (auth))
    {
      username = data->proxy_user;
      password = retrying ? NULL : data->proxy_password;
    }
  else
    {
      username = data->username;
      password = retrying ? NULL : data->password;
    }

  if (username && password)
    {
      soup_auth_authenticate (auth, username, password);
      return;
    }

  if (soup_auth_is_for_proxy (auth))
    {
      prompt = g_strdup (_("Please enter proxy password"));
    }
  else
    {
      const char *auth_realm;

      auth_realm = soup_auth_get_realm (auth);

      if (auth_realm == NULL)
        auth_realm = _("WebDAV share");

      prompt = g_strdup_printf (_("Enter password for %s"), auth_realm);
    }

  res = g_mount_source_ask_password (data->mount_source,
                                     prompt,
                                     username,
                                     NULL,
                                     G_ASK_PASSWORD_NEED_PASSWORD |
                                     G_ASK_PASSWORD_NEED_USERNAME,
                                     &aborted,
                                     &new_password,
                                     &new_username,
                                     NULL,
                                     NULL);

  if (res && !aborted) {

    soup_auth_authenticate (auth, new_username, new_password);

    if (soup_auth_is_for_proxy (auth))
      {
        g_free (data->proxy_user);
        g_free (data->proxy_password);

        data->proxy_password = new_password;
        data->proxy_user = new_username;
      }
    else
      {
        g_free (data->username);
        g_free (data->password);

        data->username = new_username;
        data->password = new_password;
      }
  }

  g_print ("- soup_authenticate \n");
  g_free (prompt);
}

static inline GMountSpec *
g_mount_spec_dup_known (GMountSpec *spec)
{
  static const char *known_keys[] = {"host", "user", "port", "ssl", NULL};
  GMountSpec *new_spec;
  const char **iter;
  const char *value;
  const char *type;

  type = g_mount_spec_get_type (spec);
  new_spec = g_mount_spec_new (type);

  for (iter = known_keys; *iter; iter++)
    {
      value = g_mount_spec_get (spec, *iter);

      if (value)
        g_mount_spec_set (new_spec, *iter, value);
    }

  return new_spec;
}

static void
do_mount (GVfsBackend  *backend,
          GVfsJobMount *job,
          GMountSpec   *mount_spec,
          GMountSource *mount_source,
          gboolean      is_automount)
{
  MountAuthInfo  *info;
  SoupSession    *session;
  SoupMessage    *msg;
  SoupURI        *mount_base;
  const char     *host;
  const char     *user;
  const char     *port;
  const char     *ssl;
  guint           port_num;
  gulong          signal_id;
  guint           status;
  gboolean        is_success;
  gboolean        is_dav;
  char           *last_good_path;

  g_print ("+ mount\n");

  host = g_mount_spec_get (mount_spec, "host");
  user = g_mount_spec_get (mount_spec, "user");
  port = g_mount_spec_get (mount_spec, "port");
  ssl  = g_mount_spec_get (mount_spec, "ssl");
  
  if (host == NULL || *host == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      
      return;
    }

  mount_base = soup_uri_new (NULL);

  if (ssl != NULL && (strcmp (ssl, "true") == 0))
    soup_uri_set_scheme (mount_base, SOUP_URI_SCHEME_HTTPS);
  else
    soup_uri_set_scheme (mount_base, SOUP_URI_SCHEME_HTTP);

  soup_uri_set_user (mount_base, user);

  if (port && (port_num = atoi (port)))
    soup_uri_set_port (mount_base, port_num);

  soup_uri_set_host (mount_base, host);
  soup_uri_set_path (mount_base, mount_spec->mount_prefix);

  session = G_VFS_BACKEND_HTTP (backend)->session;
  G_VFS_BACKEND_HTTP (backend)->mount_base = mount_base; 

  info = &(G_VFS_BACKEND_DAV (backend)->auth_info); 
  info->mount_source = g_object_ref (mount_source);
  info->username = g_strdup (user);

  signal_id = g_signal_connect (session, "authenticate",
                                G_CALLBACK (soup_authenticate_interactive),
                                info);

  last_good_path = NULL;
  msg = message_new_from_uri (SOUP_METHOD_OPTIONS, mount_base);

  do {
    status = soup_session_send_message (session, msg);

    is_success = SOUP_STATUS_IS_SUCCESSFUL (status);
    is_dav = sm_has_header (msg, "DAV");

    soup_message_headers_clear (msg->response_headers);
    soup_message_body_truncate (msg->response_body);

    if (is_success && is_dav)
      {
        g_free (last_good_path);
        last_good_path = mount_base->path;
        mount_base->path = path_get_parent_dir (mount_base->path);
        soup_message_set_uri (msg, mount_base);
      }

  } while (is_success && is_dav);

  /* we have reached the end of paths we are allowed to
   * chdir up to (or couldn't chdir up at all) */

  /* check if we at all have a good path */
  if (last_good_path == NULL) 
    {
      if (!is_success) 
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,G_IO_ERROR_FAILED,
                          _("HTTP Error: %s"), msg->reason_phrase);
      else
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,G_IO_ERROR_FAILED,
                          _("Not a WebDAV enabled share"));
      return;
    }

  /* Set the working path in mount path */
  g_free (mount_base->path);
  mount_base->path = last_good_path;

  /* dup the mountspec, but only copy known fields */
  mount_spec = g_mount_spec_dup_known (mount_spec);
  g_mount_spec_set_mount_prefix (mount_spec, mount_base->path);

  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (backend), mount_spec);
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");

  /* cleanup */
  g_mount_spec_unref (mount_spec);
  g_object_unref (msg);

  /* switch the signal handler */
  g_signal_handler_disconnect (session, signal_id);
  g_signal_connect (session, "authenticate",
                    G_CALLBACK (soup_authenticate_from_data),
                    info);

  /* also auth the workaround async session we need for SoupInputStream */
  g_signal_connect (G_VFS_BACKEND_HTTP (backend)->session_async, "authenticate",
                    G_CALLBACK (soup_authenticate_from_data),
                    info);



  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_print ("- mount\n");
}

/* *** query_info () *** */
static void
do_query_info (GVfsBackend           *backend,
               GVfsJobQueryInfo      *job,
               const char            *filename,
               GFileQueryInfoFlags    flags,
               GFileInfo             *info,
               GFileAttributeMatcher *matcher)
{
  SoupMessage *msg;
  Multistatus  ms;
  xmlNodeIter  iter;
  SoupURI     *base;
  gboolean     res;
  GError      *error;

  base    = G_VFS_BACKEND_HTTP (backend)->mount_base;
  error   = NULL;

  msg = propfind_request_new (backend, filename, 0, NULL);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Could not create request"));
      
      return;
    }

  message_add_apply_to_redirect_header (msg, flags);

  soup_session_send_message (G_VFS_BACKEND_HTTP (backend)->session, msg);

  res = multistatus_parse (msg, &ms, &error);
  g_object_unref (msg);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  res = FALSE;
  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse response;

      if (! multistatus_get_response (&iter, &response))
        continue;

      if (ms_response_is_target (&response))
        {
          ms_response_to_file_info (&response, job->file_info);
          res = TRUE;
        }
    }

  multistatus_free (&ms);

  if (res)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR,G_IO_ERROR_FAILED,
                      _("Response invalid"));

}


/* *** enumerate *** */
static void
do_enumerate (GVfsBackend           *backend,
              GVfsJobEnumerate      *job,
              const char            *filename,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags    flags)
{
  GVfsBackendHttp *backend_http;
  SoupMessage     *msg;
  Multistatus      ms;
  xmlNodeIter      iter;
  gboolean         res;
  SoupURI         *base;
  GError          *error;
 
  backend_http = G_VFS_BACKEND_HTTP (backend);
  base         = backend_http->mount_base;
  error        = NULL;

  msg = propfind_request_new (backend, filename, 1, NULL);

  if (msg == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,G_IO_ERROR_FAILED,
                        _("Could not create request"));
      
      return;
    }

  message_add_apply_to_redirect_header (msg, flags);

  soup_session_send_message (backend_http->session, msg);

  res = multistatus_parse (msg, &ms, &error);
  g_object_unref (msg);

  if (res == FALSE)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  multistatus_get_response_iter (&ms, &iter);

  while (xml_node_iter_next (&iter))
    {
      MsResponse  response;
      const char *basename;
      GFileInfo  *info;

      if (! multistatus_get_response (&iter, &response))
        continue;

      basename = ms_response_get_basename (&response);

      if (ms_response_is_target (&response))
        continue;

      info = g_file_info_new ();
      ms_response_to_file_info (&response, info);
      g_vfs_job_enumerate_add_info (job, info);
    }

  multistatus_free (&ms);

  g_vfs_job_succeeded (G_VFS_JOB (job)); /* should that be called earlier? */
  g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
}

/* ************************************************************************* */
/*  */

/* *** create () *** */
static void
try_create_tested_existence (SoupSession *session, SoupMessage *msg,
                             gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;
  GOutputStream   *stream;
  SoupMessage     *put_msg;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      return;
    }
  /* FIXME: other errors */

  put_msg = message_new_from_uri ("PUT", soup_message_get_uri (msg));

  soup_message_headers_append (put_msg->request_headers, "If-None-Match", "*");
  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}  

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupMessage     *msg;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-None-Match: *" and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  msg = message_new_from_filename (backend, "HEAD", filename);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
  soup_session_queue_message (op_backend->session, msg,
                              try_create_tested_existence, job);
  return TRUE;
}

/* *** replace () *** */
static void
open_for_replace_succeeded (GVfsBackendHttp *op_backend, GVfsJob *job,
                            SoupURI *uri, const char *etag)
{
  SoupMessage     *put_msg;
  GOutputStream   *stream;

  put_msg = message_new_from_uri (SOUP_METHOD_PUT, uri);

  if (etag)
    soup_message_headers_append (put_msg->request_headers, "If-Match", etag);

  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}

static void
try_replace_checked_etag (SoupSession *session, SoupMessage *msg,
                          gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;

  if (msg->status_code == SOUP_STATUS_PRECONDITION_FAILED)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_WRONG_ETAG,
                        _("The file was externally modified"));
      return;
    }
  /* FIXME: other errors */

  open_for_replace_succeeded (op_backend, job, soup_message_get_uri (msg),
                              soup_message_headers_get (msg->request_headers, "If-Match"));
}  

static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupURI         *uri;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-Match: ..." and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  if (make_backup)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backup file creation failed"));
      return TRUE;
    }



  uri = g_vfs_backend_uri_for_filename (backend, filename, FALSE);

  if (etag)
    {
      SoupMessage *msg;

      msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
      soup_uri_free (uri);
      soup_message_headers_append (msg->request_headers, "If-Match", etag);

      g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
      soup_session_queue_message (op_backend->session, msg,
                                  try_replace_checked_etag, job);
      return TRUE;
    }

  open_for_replace_succeeded (op_backend, G_VFS_JOB (job), uri, NULL);
  soup_uri_free (uri);
  return TRUE;
}

/* *** write () *** */
static void
write_ready (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gssize         nwrote;

  stream = G_OUTPUT_STREAM (source_object); 
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  nwrote = g_output_stream_write_finish (stream, result, &error);

  if (nwrote < 0)
   {
     g_vfs_job_failed (G_VFS_JOB (job),
                       error->domain,
                       error->code,
                       error->message);

     g_error_free (error);
     return;
   }

  g_vfs_job_write_set_written_size (G_VFS_JOB_WRITE (job), nwrote);
  g_vfs_job_succeeded (job);
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle handle,
           char *buffer,
           gsize buffer_size)
{
  GOutputStream   *stream;

  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_write_async (stream,
                               buffer,
                               buffer_size,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               write_ready,
                               job);
  return TRUE;
}

/* *** close_write () *** */
static void
close_write_ready (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gboolean       res;

  job = G_VFS_JOB (user_data);
  stream = G_OUTPUT_STREAM (source_object);
  res = g_output_stream_close_finish (stream,
                                      result,
                                      &error);
  if (res == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        error->domain,
                        error->code,
                        error->message);

      g_error_free (error);
    }
  else
    g_vfs_job_succeeded (job);

  g_object_unref (stream);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GOutputStream   *stream;

  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_close_async (stream,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               close_write_ready,
                               job);

  return TRUE;
}

static void
do_make_directory (GVfsBackend          *backend,
                   GVfsJobMakeDirectory *job,
                   const char           *filename)
{
  SoupMessage     *msg;
  guint            status;
  char            *to_free;

  msg = message_new_from_filename_full (backend, "MKCOL", filename, TRUE);

  status = soup_session_send_message (G_VFS_BACKEND_HTTP (backend)->session, msg);

  /* TODO: error reporting sucks */
  if (! SOUP_STATUS_IS_SUCCESSFUL (status))
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR,G_IO_ERROR_FAILED,
                      _("HTTP Error: %s"), msg->reason_phrase);
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  g_object_unref (msg);
}


/* ************************************************************************* */
/*  */

static void
g_vfs_backend_dav_class_init (GVfsBackendDavClass *klass)
{
  GObjectClass     *gobject_class;
  GVfsBackendClass *backend_class;
  
  gobject_class = G_OBJECT_CLASS (klass); 
  gobject_class->finalize  = g_vfs_backend_dav_finalize;

  backend_class = G_VFS_BACKEND_CLASS (klass); 

  backend_class->try_mount         = NULL;
  backend_class->mount             = do_mount;
  backend_class->try_query_info    = NULL;
  backend_class->query_info        = do_query_info;
  backend_class->enumerate         = do_enumerate;
  backend_class->try_create        = try_create;
  backend_class->try_replace       = try_replace;
  backend_class->try_write         = try_write;
  backend_class->try_close_write   = try_close_write;
  backend_class->make_directory    = do_make_directory;
}
