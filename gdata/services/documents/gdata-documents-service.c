#include <libsoup-3.0/libsoup/soup-message.h> /* Force include for soup_message_get_response_body_bytes */
/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * GData Client
 * Copyright (C) Thibault Saunier 2009 <saunierthibault@gmail.com>
 * Copyright (C) Philip Withnall 2010, 2014 <philip@tecnocode.co.uk>
 * Copyright (C) Red Hat, Inc. 2015, 2016
 *
 * GData Client is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * GData Client is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GData Client.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h> // Must be first

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib/guri.h>
#include <string.h>
#include <gio/gio.h> /* For G_IO_ERROR_CANCELLED and other GIO types */
#include <libsoup-3.0/libsoup/soup.h>
/* #include <libsoup-3.0/libsoup/soup-message.h> -- Now at the very top */

// Central private header, should include libsoup and other common internal deps
#include "gdata-private.h"

// Public API for this object
#include "gdata/services/documents/gdata-documents-service.h"

// Other GData internal headers
#include "gdata/services/documents/gdata-documents-property.h"
#include "gdata/services/documents/gdata-documents-utils.h"
#include "gdata/services/documents/gdata-documents-drive.h"
#include "gdata/gdata-batchable.h"
/* gdata-service.h is already included via gdata-private.h or gdata-documents-service.h chain */
#include "gdata/gdata-upload-stream.h"


GQuark
gdata_documents_service_error_quark (void)
{
	return g_quark_from_static_string ("gdata-documents-service-error-quark");
}

static void append_query_headers (GDataService *self, GDataAuthorizationDomain *domain, SoupMessage *message);
static GList *get_authorization_domains (void);
static gchar *_get_upload_uri_for_query_and_folder (GDataDocumentsUploadQuery *query,
                                                    GDataDocumentsFolder *folder) G_GNUC_WARN_UNUSED_RESULT G_GNUC_MALLOC;

_GDATA_DEFINE_AUTHORIZATION_DOMAIN (documents, "writely", "https://www.googleapis.com/auth/drive")
_GDATA_DEFINE_AUTHORIZATION_DOMAIN (spreadsheets, "wise", "https://spreadsheets.google.com/feeds/")
G_DEFINE_TYPE_WITH_CODE (GDataDocumentsService, gdata_documents_service, GDATA_TYPE_SERVICE, G_IMPLEMENT_INTERFACE (GDATA_TYPE_BATCHABLE, NULL))

static void
gdata_documents_service_class_init (GDataDocumentsServiceClass *klass)
{
	GDataServiceClass *service_class = GDATA_SERVICE_CLASS (klass);
	service_class->feed_type = GDATA_TYPE_DOCUMENTS_FEED;

	service_class->append_query_headers = append_query_headers;
	service_class->get_authorization_domains = get_authorization_domains;

	service_class->api_version = "3";
}

static void
gdata_documents_service_init (GDataDocumentsService *self)
{
	/* Nothing to see here */
}

static void
append_query_headers (GDataService *self, GDataAuthorizationDomain *domain, SoupMessage *message)
{
	g_assert (message != NULL);

	if (soup_message_get_method (message) == SOUP_METHOD_POST && soup_message_headers_get_one (soup_message_get_request_headers (message), "X-Upload-Content-Length") == NULL) {
		g_autofree gchar *upload_uri_str = NULL;
		const gchar *v3_pos;
		GUri *message_guri;

		message_guri = soup_message_get_uri (message);
		upload_uri_str = g_uri_to_string (message_guri);
		v3_pos = strstr (upload_uri_str, "://docs.google.com/feeds/upload/create-session/default/private/full");

		if (v3_pos != NULL) {
			g_autofree gchar *v2_upload_uri_str = NULL;
			g_autoptr(GUri) v2_guri = NULL;
			SoupMessageHeaders *request_headers = soup_message_get_request_headers (message);

			soup_message_headers_replace (request_headers, "X-Upload-Content-Length", "0");
			soup_message_headers_set_encoding (request_headers, SOUP_ENCODING_CONTENT_LENGTH);

			v2_upload_uri_str = g_strconcat (_gdata_service_get_scheme (), "://docs.google.com/feeds/default/private/full",
			                                 v3_pos + strlen ("://docs.google.com/feeds/upload/create-session/default/private/full"), NULL);
			v2_guri = g_uri_parse (v2_upload_uri_str, G_URI_FLAGS_NONE, NULL);
			if (v2_guri) {
				soup_message_set_uri (message, v2_guri);
			}
		}
	}

	GDATA_SERVICE_CLASS (gdata_documents_service_parent_class)->append_query_headers (self, domain, message);
}

static GList *
get_authorization_domains (void)
{
	GList *authorization_domains = NULL;

	authorization_domains = g_list_prepend (authorization_domains, get_documents_authorization_domain ());
	authorization_domains = g_list_prepend (authorization_domains, get_spreadsheets_authorization_domain ());

	return authorization_domains;
}

GDataDocumentsService *
gdata_documents_service_new (GDataAuthorizer *authorizer)
{
	g_return_val_if_fail (authorizer == NULL || GDATA_IS_AUTHORIZER (authorizer), NULL);

	return g_object_new (GDATA_TYPE_DOCUMENTS_SERVICE,
	                     "authorizer", authorizer,
	                     NULL);
}

GDataAuthorizationDomain *
gdata_documents_service_get_primary_authorization_domain (void)
{
	return get_documents_authorization_domain ();
}

GDataAuthorizationDomain *
gdata_documents_service_get_spreadsheet_authorization_domain (void)
{
	return get_spreadsheets_authorization_domain ();
}

GDataDocumentsMetadata *
gdata_documents_service_get_metadata (GDataDocumentsService *self, GCancellable *cancellable, GError **error)
{
	GDataDocumentsMetadata *metadata;
	const gchar *uri = "https://www.googleapis.com/drive/v2/about";
	SoupMessage *message;
	guint status;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	message = _gdata_service_build_message (GDATA_SERVICE (self), get_documents_authorization_domain (), SOUP_METHOD_GET, uri, NULL, FALSE);

	status = _gdata_service_send_message (GDATA_SERVICE (self), message, cancellable, error);

	if (status == SOUP_STATUS_NONE || (error != NULL && (*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED)) {
		g_object_unref (message);
		return NULL;
	} else if (status != SOUP_STATUS_OK) {
		GDataServiceClass *klass = GDATA_SERVICE_GET_CLASS (self);
		GBytes *response_bytes_ptr = NULL;
		const char *response_data = NULL;
		gsize response_length = 0;

		g_assert (klass->parse_error_response != NULL);
		response_bytes_ptr = soup_message_get_response_body_bytes (message);
		if (response_bytes_ptr) {
			response_data = g_bytes_get_data (response_bytes_ptr, &response_length);
		}
		klass->parse_error_response (GDATA_SERVICE (self), GDATA_OPERATION_QUERY, status, soup_message_get_reason_phrase (message), response_data,
					     response_length, error);
		if (response_bytes_ptr) {
			g_bytes_unref (response_bytes_ptr);
		}
		g_object_unref (message);
		return NULL;
	}

	GBytes *response_bytes_ptr = soup_message_get_response_body_bytes (message);
	gsize response_length = 0;
	const void *response_data = NULL;

	if (response_bytes_ptr) {
		response_data = g_bytes_get_data (response_bytes_ptr, &response_length);
	}
	if (response_length > 0) {
		g_assert (response_data != NULL);
	} else {
		/* Allow empty response data if length is 0 */
	}
	metadata = GDATA_DOCUMENTS_METADATA (gdata_parsable_new_from_json (GDATA_TYPE_DOCUMENTS_METADATA, response_data, response_length,
	                                                                    error));
	if (response_bytes_ptr) {
		g_bytes_unref (response_bytes_ptr);
	}
	g_object_unref (message);

	return metadata;
}

static void
get_metadata_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	GDataDocumentsService *service = GDATA_DOCUMENTS_SERVICE (source_object);
	g_autoptr(GDataDocumentsMetadata) metadata = NULL;
	g_autoptr(GError) error = NULL;

	metadata = gdata_documents_service_get_metadata (service, cancellable, &error);
	if (error != NULL)
		g_task_return_error (task, g_steal_pointer (&error));
	else
		g_task_return_pointer (task, g_steal_pointer (&metadata), g_object_unref);
}

void
gdata_documents_service_get_metadata_async (GDataDocumentsService *self, GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gdata_documents_service_get_metadata_async);
	g_task_run_in_thread (task, get_metadata_thread);
}

GDataDocumentsMetadata *
gdata_documents_service_get_metadata_finish (GDataDocumentsService *self, GAsyncResult *async_result, GError **error)
{
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (async_result), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	g_return_val_if_fail (g_task_is_valid (async_result, self), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (async_result, gdata_documents_service_get_metadata_async), NULL);

	return g_task_propagate_pointer (G_TASK (async_result), error);
}

static gchar *
_query_documents_build_request_uri (GDataDocumentsQuery *query)
{
	return g_strdup ("https://www.googleapis.com/drive/v2/files");
}

GDataDocumentsFeed *
gdata_documents_service_query_documents (GDataDocumentsService *self, GDataDocumentsQuery *query, GCancellable *cancellable,
                                         GDataQueryProgressCallback progress_callback, gpointer progress_user_data,
                                         GError **error)
{
	GDataFeed *feed;
	gchar *request_uri;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (query == NULL || GDATA_IS_DOCUMENTS_QUERY (query), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to query documents."));
		return NULL;
	}

	request_uri = _query_documents_build_request_uri (query);
	feed = gdata_service_query (GDATA_SERVICE (self), get_documents_authorization_domain (), request_uri, GDATA_QUERY (query),
	                            GDATA_TYPE_DOCUMENTS_ENTRY, cancellable, progress_callback, progress_user_data, error);
	g_free (request_uri);

	return GDATA_DOCUMENTS_FEED (feed);
}

void
gdata_documents_service_query_documents_async (GDataDocumentsService *self, GDataDocumentsQuery *query, GCancellable *cancellable,
                                               GDataQueryProgressCallback progress_callback, gpointer progress_user_data,
                                               GDestroyNotify destroy_progress_user_data,
                                               GAsyncReadyCallback callback, gpointer user_data)
{
	gchar *request_uri;

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (query == NULL || GDATA_IS_DOCUMENTS_QUERY (query));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
	g_return_if_fail (callback != NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_autoptr(GTask) task = NULL;

		task = g_task_new (self, cancellable, callback, user_data);
		g_task_set_source_tag (task, gdata_service_query_async);
		g_task_return_new_error (task, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED, "%s",
		                         _("You must be authenticated to query documents."));

		return;
	}

	request_uri = _query_documents_build_request_uri (query);
	gdata_service_query_async (GDATA_SERVICE (self), get_documents_authorization_domain (), request_uri, GDATA_QUERY (query),
	                           GDATA_TYPE_DOCUMENTS_ENTRY, cancellable, progress_callback, progress_user_data,
	                           destroy_progress_user_data, callback, user_data);
	g_free (request_uri);
}

GDataDocumentsFeed *
gdata_documents_service_query_drives (GDataDocumentsService *self, GDataDocumentsDriveQuery *query, GCancellable *cancellable,
                                      GDataQueryProgressCallback progress_callback, gpointer progress_user_data,
                                      GError **error)
{
	GDataFeed *feed;
	const gchar *request_uri = "https://www.googleapis.com/drive/v2/drives";

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (query == NULL || GDATA_IS_DOCUMENTS_DRIVE_QUERY (query), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to query drives."));
		return NULL;
	}

	feed = gdata_service_query (GDATA_SERVICE (self), get_documents_authorization_domain (), request_uri, GDATA_QUERY (query),
	                            GDATA_TYPE_DOCUMENTS_DRIVE, cancellable, progress_callback, progress_user_data, error);

	return GDATA_DOCUMENTS_FEED (feed);
}

void
gdata_documents_service_query_drives_async (GDataDocumentsService *self, GDataDocumentsDriveQuery *query, GCancellable *cancellable,
                                            GDataQueryProgressCallback progress_callback, gpointer progress_user_data,
                                            GDestroyNotify destroy_progress_user_data,
                                            GAsyncReadyCallback callback, gpointer user_data)
{
	const gchar *request_uri = "https://www.googleapis.com/drive/v2/drives";

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (query == NULL || GDATA_IS_DOCUMENTS_QUERY (query));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
	g_return_if_fail (callback != NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_autoptr(GTask) task = NULL;

		task = g_task_new (self, cancellable, callback, user_data);
		g_task_set_source_tag (task, gdata_service_query_async);
		g_task_return_new_error (task, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED, "%s",
		                         _("You must be authenticated to query drives."));

		return;
	}

	gdata_service_query_async (GDATA_SERVICE (self), get_documents_authorization_domain (), request_uri, GDATA_QUERY (query),
	                           GDATA_TYPE_DOCUMENTS_DRIVE, cancellable, progress_callback, progress_user_data,
	                           destroy_progress_user_data, callback, user_data);
}

static void
add_folder_link_to_entry (GDataDocumentsEntry *entry, GDataDocumentsFolder *folder)
{
	GDataLink *_link;
	const gchar *id;
	gchar *uri;

	id = gdata_entry_get_id (GDATA_ENTRY (folder));
	uri = g_strconcat (GDATA_DOCUMENTS_URI_PREFIX, id, NULL);
	_link = gdata_link_new (uri, GDATA_LINK_PARENT);
	gdata_entry_add_link (GDATA_ENTRY (entry), _link);
	g_object_unref (_link);
	g_free (uri);
}

static GDataUploadStream *
upload_update_document (GDataDocumentsService *self, GDataDocumentsDocument *document, const gchar *slug, const gchar *content_type,
                        GDataDocumentsFolder *folder, goffset content_length, const gchar *method, const gchar *upload_uri,
			GCancellable *cancellable)
{
	if (folder != NULL)
		add_folder_link_to_entry (GDATA_DOCUMENTS_ENTRY (document), folder);

	if (content_length == -1) {
		return GDATA_UPLOAD_STREAM (gdata_upload_stream_new (GDATA_SERVICE (self), get_documents_authorization_domain (), method, upload_uri,
	                                                             GDATA_ENTRY (document), slug, content_type, cancellable));
	} else {
		return GDATA_UPLOAD_STREAM (gdata_upload_stream_new_resumable (GDATA_SERVICE (self), get_documents_authorization_domain (), method,
		                                                               upload_uri, GDATA_ENTRY (document), slug, content_type, content_length,
		                                                               cancellable));
	}
}

static gboolean
_upload_checks (GDataDocumentsService *self, GDataDocumentsDocument *document, GError **error)
{
	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to upload documents."));
		return FALSE;
	}

	if (document != NULL && gdata_entry_is_inserted (GDATA_ENTRY (document)) == TRUE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_ENTRY_ALREADY_INSERTED,
		                     _("The document has already been uploaded."));
		return FALSE;
	}

	return TRUE;
}

GDataUploadStream *
gdata_documents_service_upload_document (GDataDocumentsService *self, GDataDocumentsDocument *document, const gchar *slug, const gchar *content_type,
                                         GDataDocumentsFolder *folder, GCancellable *cancellable, GError **error)
{
	GDataUploadStream *upload_stream;
	gchar *upload_uri;
	gchar *upload_uri_prefix;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (document == NULL || GDATA_IS_DOCUMENTS_DOCUMENT (document), NULL);
	g_return_val_if_fail (slug != NULL && *slug != '\0', NULL);
	g_return_val_if_fail (content_type != NULL && *content_type != '\0', NULL);
	g_return_val_if_fail (folder == NULL || GDATA_IS_DOCUMENTS_FOLDER (folder), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (_upload_checks (self, document, error) == FALSE) {
		return NULL;
	}

	upload_uri_prefix = gdata_documents_service_get_upload_uri (folder);
	upload_uri = g_strconcat (upload_uri_prefix, "?uploadType=multipart", NULL);
	upload_stream = upload_update_document (self, document, slug, content_type, folder, -1, SOUP_METHOD_POST, upload_uri, cancellable);
	g_free (upload_uri);
	g_free (upload_uri_prefix);

	return upload_stream;
}

GDataUploadStream *
gdata_documents_service_upload_document_resumable (GDataDocumentsService *self, GDataDocumentsDocument *document, const gchar *slug,
                                                   const gchar *content_type, goffset content_length, GDataDocumentsUploadQuery *query,
                                                   GCancellable *cancellable, GError **error)
{
	GDataUploadStream *upload_stream;
	gchar *upload_uri;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (document == NULL || GDATA_IS_DOCUMENTS_DOCUMENT (document), NULL);
	g_return_val_if_fail (slug != NULL && *slug != '\0', NULL);
	g_return_val_if_fail (content_type != NULL && *content_type != '\0', NULL);
	g_return_val_if_fail (query == NULL || GDATA_IS_DOCUMENTS_UPLOAD_QUERY (query), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (_upload_checks (self, document, error) == FALSE) {
		return NULL;
	}

	upload_uri = _get_upload_uri_for_query_and_folder (query, NULL);
	upload_stream = upload_update_document (self, document, slug, content_type, NULL, content_length, SOUP_METHOD_POST, upload_uri, cancellable);
	g_free (upload_uri);

	return upload_stream;
}

static gboolean
_update_checks (GDataDocumentsService *self, GError **error)
{
	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to update documents."));
		return FALSE;
	}

	return TRUE;
}

GDataUploadStream *
gdata_documents_service_update_document (GDataDocumentsService *self, GDataDocumentsDocument *document, const gchar *slug, const gchar *content_type,
                                         GCancellable *cancellable, GError **error)
{
	GDataUploadStream *update_stream;
	const gchar *id;
	gchar *update_uri;
	gchar *update_uri_prefix;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_DOCUMENT (document), NULL);
	g_return_val_if_fail (slug != NULL && *slug != '\0', NULL);
	g_return_val_if_fail (content_type != NULL && *content_type != '\0', NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (_update_checks (self, error) == FALSE) {
		return NULL;
	}

	update_uri_prefix = gdata_documents_service_get_upload_uri (NULL);
	id = gdata_entry_get_id (GDATA_ENTRY (document));
	update_uri = g_strconcat (update_uri_prefix, "/", id, "?uploadType=multipart", NULL);
	update_stream = upload_update_document (self, document, slug, content_type, NULL, -1, SOUP_METHOD_PUT, update_uri, cancellable);
	g_free (update_uri);
	g_free (update_uri_prefix);

	return update_stream;
}

GDataUploadStream *
gdata_documents_service_update_document_resumable (GDataDocumentsService *self, GDataDocumentsDocument *document, const gchar *slug,
                                                   const gchar *content_type, goffset content_length, GCancellable *cancellable, GError **error)
{
	GDataLink *update_link;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_DOCUMENT (document), NULL);
	g_return_val_if_fail (slug != NULL && *slug != '\0', NULL);
	g_return_val_if_fail (content_type != NULL && *content_type != '\0', NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (_update_checks (self, error) == FALSE) {
		return NULL;
	}

	update_link = gdata_entry_look_up_link (GDATA_ENTRY (document), GDATA_LINK_RESUMABLE_EDIT_MEDIA);
	g_assert (update_link != NULL);

	return upload_update_document (self, document, slug, content_type, NULL, content_length, SOUP_METHOD_PUT, gdata_link_get_uri (update_link),
	                               cancellable);
}

GDataDocumentsDocument *
gdata_documents_service_finish_upload (GDataDocumentsService *self, GDataUploadStream *upload_stream, GError **error)
{
	const gchar *content_type;
	const gchar *response_body;
	gssize response_length;
	GType new_document_type = G_TYPE_INVALID;

	response_body = gdata_upload_stream_get_response (upload_stream, &response_length);
	if (response_body == NULL || response_length == 0) {
		return NULL;
	}

	content_type = gdata_upload_stream_get_content_type (upload_stream);
	new_document_type = gdata_documents_utils_get_type_from_content_type (content_type);

	if (g_type_is_a (new_document_type, GDATA_TYPE_DOCUMENTS_DOCUMENT) == FALSE) {
		g_set_error (error, GDATA_DOCUMENTS_SERVICE_ERROR, GDATA_DOCUMENTS_SERVICE_ERROR_INVALID_CONTENT_TYPE,
		             _("The content type of the supplied document (‘%s’) could not be recognized."),
		             content_type);
		return NULL;
	}

	return GDATA_DOCUMENTS_DOCUMENT (gdata_parsable_new_from_json (new_document_type, response_body, (gint) response_length, error));
}

GDataDocumentsDocument *
gdata_documents_service_copy_document (GDataDocumentsService *self, GDataDocumentsDocument *document, GCancellable *cancellable, GError **error)
{
	GDataDocumentsDocument *new_document;
	GDataEntry *parent = NULL;
	GList *i;
	GList *parent_folders_list;
	const gchar *parent_id = NULL;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_DOCUMENT (document), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to copy documents."));
		return NULL;
	}

	parent_folders_list = gdata_entry_look_up_links (GDATA_ENTRY (document), GDATA_LINK_PARENT);
	for (i = parent_folders_list; i != NULL; i = i->next) {
		GDataLink *_link = GDATA_LINK (i->data);
		const gchar *id;

		id = gdata_documents_utils_get_id_from_link (_link);
		if (id != NULL) {
			parent_id = id;
			break;
		}
	}

	g_list_free (parent_folders_list);

	if (parent_id == NULL) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_NOT_FOUND, _("Parent folder not found"));
		return NULL;
	}

	parent = gdata_service_query_single_entry (GDATA_SERVICE (self), get_documents_authorization_domain (), parent_id, NULL, GDATA_TYPE_DOCUMENTS_FOLDER, cancellable, error);
	if (parent == NULL)
		return NULL;

	new_document = GDATA_DOCUMENTS_DOCUMENT (gdata_documents_service_add_entry_to_folder (self, GDATA_DOCUMENTS_ENTRY (document), GDATA_DOCUMENTS_FOLDER (parent), cancellable, error));
	g_object_unref (parent);

	return new_document;
}

static void
copy_document_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	GDataDocumentsService *service = GDATA_DOCUMENTS_SERVICE (source_object);
	GDataDocumentsDocument *document = task_data;
	g_autoptr(GDataDocumentsDocument) new_document = NULL;
	g_autoptr(GError) error = NULL;

	new_document = gdata_documents_service_copy_document (service, document, cancellable, &error);
	if (error != NULL)
		g_task_return_error (task, g_steal_pointer (&error));
	else
		g_task_return_pointer (task, g_steal_pointer (&new_document), g_object_unref);
}

void
gdata_documents_service_copy_document_async (GDataDocumentsService *self, GDataDocumentsDocument *document, GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (GDATA_IS_DOCUMENTS_DOCUMENT (document));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gdata_documents_service_copy_document_async);
	g_task_set_task_data (task, g_object_ref (document), (GDestroyNotify) g_object_unref);
	g_task_run_in_thread (task, copy_document_thread);
}

GDataDocumentsDocument *
gdata_documents_service_copy_document_finish (GDataDocumentsService *self, GAsyncResult *async_result, GError **error)
{
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (async_result), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	g_return_val_if_fail (g_task_is_valid (async_result, self), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (async_result, gdata_documents_service_copy_document_async), NULL);

	return g_task_propagate_pointer (G_TASK (async_result), error);
}

GDataDocumentsEntry *
gdata_documents_service_add_entry_to_folder (GDataDocumentsService *self, GDataDocumentsEntry *entry, GDataDocumentsFolder *folder,
                                             GCancellable *cancellable, GError **error)
{
	GDataDocumentsEntry *new_entry;
	GDataDocumentsEntry *local_entry;
	GDataOperationType operation_type;
	GType entry_type;
	const gchar *content_type;
	const gchar *etag;
	const gchar *title;
	const gchar *uri_prefix = "https://www.googleapis.com/drive/v2/files";
	gchar *upload_data;
	gchar *uri;
	SoupMessage *message;
	guint status;
	GList *l;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_ENTRY (entry), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_FOLDER (folder), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to insert or move documents and folders."));
		return NULL;
	}

	if (gdata_entry_is_inserted (GDATA_ENTRY (entry)) == TRUE) {
		const gchar *id;

		id = gdata_entry_get_id (GDATA_ENTRY (entry));
		uri = g_strconcat (uri_prefix, "/", id, "/copy", NULL);
		operation_type = GDATA_OPERATION_UPDATE;
	} else {
		uri = g_strdup (uri_prefix);
		operation_type = GDATA_OPERATION_INSERTION;
	}

	entry_type = G_OBJECT_TYPE (entry);
	content_type = gdata_documents_utils_get_content_type (entry);
	etag = gdata_entry_get_etag (GDATA_ENTRY (entry));
	title = gdata_entry_get_title (GDATA_ENTRY (entry));
	local_entry = g_object_new (entry_type, "etag", etag, "title", title, NULL);
	gdata_documents_utils_add_content_type (local_entry, content_type);
	add_folder_link_to_entry (local_entry, folder);

	for (l = gdata_documents_entry_get_document_properties (entry); l != NULL; l = l->next) {
		GDataDocumentsProperty *old_prop;
		g_autoptr(GDataDocumentsProperty) new_prop = NULL;

		old_prop = GDATA_DOCUMENTS_PROPERTY (l->data);

		new_prop = gdata_documents_property_new (gdata_documents_property_get_key (old_prop));
		gdata_documents_property_set_value (new_prop, gdata_documents_property_get_value (old_prop));
		gdata_documents_property_set_visibility (new_prop, gdata_documents_property_get_visibility (old_prop));
		gdata_documents_entry_add_documents_property (local_entry, new_prop);
	}

	message = _gdata_service_build_message (GDATA_SERVICE (self), get_documents_authorization_domain (), SOUP_METHOD_POST, uri, NULL, FALSE);
	g_free (uri);

	upload_data = gdata_parsable_get_json (GDATA_PARSABLE (local_entry));
	soup_message_set_request_body_from_bytes (message, "application/json", g_bytes_new_take (upload_data, strlen (upload_data)));
	g_object_unref (local_entry);

	status = _gdata_service_send_message (GDATA_SERVICE (self), message, cancellable, error);

	if (status == SOUP_STATUS_NONE || (error != NULL && (*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED)) {
		g_object_unref (message);
		return NULL;
	} else if (status != SOUP_STATUS_OK) {
		GDataServiceClass *klass = GDATA_SERVICE_GET_CLASS (self);
		GBytes *response_bytes_ptr = NULL;
		const char *response_data = NULL;
		gsize response_length = 0;

		g_assert (klass->parse_error_response != NULL);
		response_bytes_ptr = soup_message_get_response_body_bytes (message);
		if (response_bytes_ptr) {
			response_data = g_bytes_get_data (response_bytes_ptr, &response_length);
		}
		klass->parse_error_response (GDATA_SERVICE (self), operation_type, status, soup_message_get_reason_phrase (message), response_data,
					     response_length, error);
		if (response_bytes_ptr) {
			g_bytes_unref (response_bytes_ptr);
		}
		g_object_unref (message);
		return NULL;
	}

	GBytes *response_bytes_ptr = soup_message_get_response_body_bytes (message);
	gsize response_length = 0;
	const void *response_data = NULL;

	if (response_bytes_ptr) {
		response_data = g_bytes_get_data (response_bytes_ptr, &response_length);
	}
	if (response_length > 0) {
		g_assert (response_data != NULL);
	} else {
		/* Allow empty response data if length is 0 */
	}
	new_entry = GDATA_DOCUMENTS_ENTRY (gdata_parsable_new_from_json (entry_type, response_data, response_length,
									 error));
	if (response_bytes_ptr) {
		g_bytes_unref (response_bytes_ptr);
	}
	g_object_unref (message);

	return new_entry;
}

typedef struct {
	GDataDocumentsEntry *entry;
	GDataDocumentsFolder *folder;
} AddEntryToFolderData;

static void
add_entry_to_folder_data_free (AddEntryToFolderData *data)
{
	g_object_unref (data->entry);
	g_object_unref (data->folder);
	g_slice_free (AddEntryToFolderData, data);
}

static void
add_entry_to_folder_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	GDataDocumentsService *service = GDATA_DOCUMENTS_SERVICE (source_object);
	g_autoptr(GDataDocumentsEntry) updated_entry = NULL;
	AddEntryToFolderData *data = task_data;
	g_autoptr(GError) error = NULL;

	updated_entry = gdata_documents_service_add_entry_to_folder (service, data->entry, data->folder, cancellable, &error);
	if (error != NULL)
		g_task_return_error (task, g_steal_pointer (&error));
	else
		g_task_return_pointer (task, g_steal_pointer (&updated_entry), (GDestroyNotify) g_object_unref);
}

void
gdata_documents_service_add_entry_to_folder_async (GDataDocumentsService *self, GDataDocumentsEntry *entry, GDataDocumentsFolder *folder,
                                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	AddEntryToFolderData *data;

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (GDATA_IS_DOCUMENTS_ENTRY (entry));
	g_return_if_fail (GDATA_IS_DOCUMENTS_FOLDER (folder));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	data = g_slice_new (AddEntryToFolderData);
	data->entry = g_object_ref (entry);
	data->folder = g_object_ref (folder);

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gdata_documents_service_add_entry_to_folder_async);
	g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify) add_entry_to_folder_data_free);
	g_task_run_in_thread (task, add_entry_to_folder_thread);
}

GDataDocumentsEntry *
gdata_documents_service_add_entry_to_folder_finish (GDataDocumentsService *self, GAsyncResult *async_result, GError **error)
{
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (async_result), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	g_return_val_if_fail (g_task_is_valid (async_result, self), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (async_result, gdata_documents_service_add_entry_to_folder_async), NULL);

	return g_task_propagate_pointer (G_TASK (async_result), error);
}

GDataDocumentsEntry *
gdata_documents_service_remove_entry_from_folder (GDataDocumentsService *self, GDataDocumentsEntry *entry, GDataDocumentsFolder *folder,
                                                  GCancellable *cancellable, GError **error)
{
	const gchar *folder_id;
	GList *i;
	GList *parent_folders_list;
	GDataLink *folder_link = NULL, *file_link = NULL;
	GDataParsableClass *klass;
	GDataAuthorizationDomain *domain;
	gchar *fixed_uri, *modified_uri;
	guint status;
	gboolean req_status = TRUE;
	SoupMessage *message;

	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_ENTRY (entry), NULL);
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_FOLDER (folder), NULL);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (gdata_authorizer_is_authorized_for_domain (gdata_service_get_authorizer (GDATA_SERVICE (self)),
	                                               get_documents_authorization_domain ()) == FALSE) {
		g_set_error_literal (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED,
		                     _("You must be authenticated to move documents and folders."));
		return NULL;
	}

	domain = gdata_documents_service_get_primary_authorization_domain();

	folder_id = gdata_entry_get_id (GDATA_ENTRY (folder));
	g_assert (folder_id != NULL);

	parent_folders_list = gdata_entry_look_up_links (GDATA_ENTRY (entry), GDATA_LINK_PARENT);
	for (i = parent_folders_list; i != NULL; i = i->next) {
		GDataLink *_link = GDATA_LINK (i->data);
		const gchar *id;

		id = gdata_documents_utils_get_id_from_link (_link);
		if (g_strcmp0 (folder_id, id) == 0) {
			folder_link = _link;
			break;
		}
	}

	g_list_free (parent_folders_list);

	if (folder_link == NULL) {
		g_set_error_literal (error,
				     GDATA_SERVICE_ERROR,
				     GDATA_SERVICE_ERROR_NOT_FOUND,
				     _("Parent folder not found"));
		return NULL;
	}

	klass = GDATA_PARSABLE_GET_CLASS (entry);

	g_assert (klass->get_content_type != NULL);
	if (g_strcmp0 (klass->get_content_type (), "application/json") == 0) {
		file_link = gdata_entry_look_up_link (GDATA_ENTRY (entry), GDATA_LINK_SELF);
	} else {
		file_link = gdata_entry_look_up_link (GDATA_ENTRY (entry), GDATA_LINK_EDIT);
	}
	g_debug ("Link = %s", gdata_link_get_uri(file_link));
	g_assert (file_link != NULL);

	fixed_uri = _gdata_service_fix_uri_scheme (gdata_link_get_uri (file_link));
	modified_uri = g_strconcat (fixed_uri, "/parents/", folder_id, NULL);

	message = _gdata_service_build_message (GDATA_SERVICE (self),
						domain,
						SOUP_METHOD_DELETE,
						modified_uri,
						gdata_entry_get_etag (GDATA_ENTRY (entry)),
						TRUE);
	g_free (fixed_uri);
	g_free (modified_uri);

	status = _gdata_service_send_message (GDATA_SERVICE (self), message, cancellable, error);

	if (status == SOUP_STATUS_NONE || (error != NULL && (*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED)) {
		g_object_unref (message);
		return NULL;
	} else if (status != SOUP_STATUS_OK && status != SOUP_STATUS_NO_CONTENT) {
		GDataServiceClass *service_klass = GDATA_SERVICE_GET_CLASS (self);
		GBytes *response_bytes_ptr = NULL;
		const char *response_data = NULL;
		gsize response_length = 0;

		g_assert (service_klass->parse_error_response != NULL);
		response_bytes_ptr = soup_message_get_response_body_bytes (message);
		if (response_bytes_ptr) {
			response_data = g_bytes_get_data (response_bytes_ptr, &response_length);
		}
		service_klass->parse_error_response (GDATA_SERVICE (self),
						     GDATA_OPERATION_DELETION,
						     status,
						     soup_message_get_reason_phrase (message),
						     response_data,
						     response_length,
						     error);
		if (response_bytes_ptr) {
			g_bytes_unref (response_bytes_ptr);
		}
		req_status = FALSE;
	}

	g_object_unref (message);

	if (req_status) {
		gdata_entry_remove_link (GDATA_ENTRY (entry), folder_link);
		g_object_ref (entry);
		return entry;
	} else {
		return NULL;
	}
}

typedef struct {
	GDataDocumentsEntry *entry;
	GDataDocumentsFolder *folder;
} RemoveEntryFromFolderData;

static void
remove_entry_from_folder_data_free (RemoveEntryFromFolderData *data)
{
	g_object_unref (data->entry);
	g_object_unref (data->folder);
	g_slice_free (RemoveEntryFromFolderData, data);
}

static void
remove_entry_from_folder_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	GDataDocumentsService *service = GDATA_DOCUMENTS_SERVICE (source_object);
	g_autoptr(GDataDocumentsEntry) updated_entry = NULL;
	RemoveEntryFromFolderData *data = task_data;
	g_autoptr(GError) error = NULL;

	updated_entry = gdata_documents_service_remove_entry_from_folder (service, data->entry, data->folder, cancellable, &error);
	if (error != NULL)
		g_task_return_error (task, g_steal_pointer (&error));
	else
		g_task_return_pointer (task, g_steal_pointer (&updated_entry), g_object_unref);
}

void
gdata_documents_service_remove_entry_from_folder_async (GDataDocumentsService *self, GDataDocumentsEntry *entry, GDataDocumentsFolder *folder,
                                                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	RemoveEntryFromFolderData *data;

	g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self));
	g_return_if_fail (GDATA_IS_DOCUMENTS_ENTRY (entry));
	g_return_if_fail (GDATA_IS_DOCUMENTS_FOLDER (folder));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	data = g_slice_new (RemoveEntryFromFolderData);
	data->entry = g_object_ref (entry);
	data->folder = g_object_ref (folder);

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gdata_documents_service_remove_entry_from_folder_async);
	g_task_set_task_data (task, g_steal_pointer (&data), (GDestroyNotify) remove_entry_from_folder_data_free);
	g_task_run_in_thread (task, remove_entry_from_folder_thread);
}

GDataDocumentsEntry *
gdata_documents_service_remove_entry_from_folder_finish (GDataDocumentsService *self, GAsyncResult *async_result, GError **error)
{
	g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (async_result), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);
	g_return_val_if_fail (g_task_is_valid (async_result, self), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (async_result, gdata_documents_service_remove_entry_from_folder_async), NULL);

	return g_task_propagate_pointer (G_TASK (async_result), error);
}

static gchar *
_get_upload_uri_for_query_and_folder (GDataDocumentsUploadQuery *query, GDataDocumentsFolder *folder)
{
	if (query == NULL) {
		query = gdata_documents_upload_query_new ();
	}

	if (folder != NULL) {
		gdata_documents_upload_query_set_folder (query, folder);
	}

	return gdata_documents_upload_query_build_uri (query);
}

gchar *
gdata_documents_service_get_upload_uri (GDataDocumentsFolder *folder)
{
	g_return_val_if_fail (folder == NULL || GDATA_IS_DOCUMENTS_FOLDER (folder), NULL);

	return g_strdup ("https://www.googleapis.com/upload/drive/v2/files");
}
