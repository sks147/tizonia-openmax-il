/**
 * Copyright (C) 2011-2014 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   httpsrcprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia OpenMAX IL - HTTP streaming client processor
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>

#include <tizplatform.h>

#include <tizkernel.h>
#include <tizscheduler.h>

#include "httpsrc.h"
#include "httpsrcprc.h"
#include "httpsrcprc_decls.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.http_source.prc"
#endif

/* forward declarations */
static OMX_ERRORTYPE httpsrc_prc_deallocate_resources (void *);
static OMX_BUFFERHEADERTYPE *buffer_needed (httpsrc_prc_t *);
static OMX_ERRORTYPE release_buffer (httpsrc_prc_t *);

/* These macros assume the existence of an "ap_prc" local variable */
#define bail_on_curl_error(expr)                                             \
  do                                                                         \
    {                                                                        \
      CURLcode curl_error = CURLE_OK;                                        \
      if (CURLE_OK != (curl_error = (expr)))                                 \
        {                                                                    \
          TIZ_ERROR (handleOf (ap_prc),                                      \
                     "[OMX_ErrorInsufficientResources] : error while using " \
                     "curl (%s)",                                            \
                     curl_easy_strerror (curl_error));                       \
          goto end;                                                          \
        }                                                                    \
    }                                                                        \
  while (0)

#define bail_on_curl_multi_error(expr)                                       \
  do                                                                         \
    {                                                                        \
      CURLMcode curl_error = CURLM_OK;                                       \
      if (CURLM_OK != (curl_error = (expr)))                                 \
        {                                                                    \
          TIZ_ERROR (handleOf (ap_prc),                                      \
                     "[OMX_ErrorInsufficientResources] : error while using " \
                     "curl multi (%s)",                                      \
                     curl_multi_strerror (curl_error));                      \
          goto end;                                                          \
        }                                                                    \
    }                                                                        \
  while (0)

#define bail_on_oom(expr)                                                    \
  do                                                                         \
    {                                                                        \
      if (NULL == (expr))                                                    \
        {                                                                    \
          TIZ_ERROR (handleOf (ap_prc), "[OMX_ErrorInsufficientResources]"); \
          goto end;                                                          \
        }                                                                    \
    }                                                                        \
  while (0)

#define on_curl_multi_error_ret_omx_oom(expr)                                \
  do                                                                         \
    {                                                                        \
      CURLMcode curl_error = CURLM_OK;                                       \
      if (CURLM_OK != (curl_error = (expr)))                                 \
        {                                                                    \
          TIZ_ERROR (handleOf (ap_prc),                                      \
                     "[OMX_ErrorInsufficientResources] : error while using " \
                     "curl multi (%s)",                                      \
                     curl_multi_strerror (curl_error));                      \
          return OMX_ErrorInsufficientResources;                             \
        }                                                                    \
    }                                                                        \
  while (0)

static inline OMX_ERRORTYPE start_io_watcher (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_ev_io_);
  ap_prc->awaiting_io_ev_ = true;
  TIZ_DEBUG (handleOf (ap_prc), "");
  return tiz_event_io_start (ap_prc->p_ev_io_);
}

static inline OMX_ERRORTYPE stop_io_watcher (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (NULL != ap_prc);
  TIZ_DEBUG (handleOf (ap_prc), "");
  if (NULL != ap_prc->p_ev_io_)
    {
      rc = tiz_event_io_stop (ap_prc->p_ev_io_);
    }
  ap_prc->awaiting_io_ev_ = false;
  return rc;
}

static inline OMX_ERRORTYPE start_timer_watcher (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_ev_timer_);
  ap_prc->awaiting_timer_ev_ = true;
  TIZ_DEBUG (handleOf (ap_prc), "");
  tiz_event_timer_set (ap_prc->p_ev_timer_, ap_prc->curl_timeout_, 0.);
  return tiz_event_timer_start (ap_prc->p_ev_timer_);
}

static inline OMX_ERRORTYPE restart_timer_watcher (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  assert (NULL != ap_prc->p_ev_timer_);
  ap_prc->awaiting_timer_ev_ = true;
  TIZ_DEBUG (handleOf (ap_prc), "");
  return tiz_event_timer_restart (ap_prc->p_ev_timer_);
}

static inline OMX_ERRORTYPE stop_timer_watcher (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (NULL != ap_prc);
  TIZ_DEBUG (handleOf (ap_prc), "");
  if (NULL != ap_prc->p_ev_timer_)
    {
      rc = tiz_event_timer_stop (ap_prc->p_ev_timer_);
    }
  ap_prc->awaiting_timer_ev_ = false;
  return rc;
}

/* This function gets called by libcurl as soon as it has received header
   data. The header callback will be called once for each header and only
   complete header lines are passed on to the callback. Parsing headers is very
   easy using this. The size of the data pointed to by ptr is size multiplied
   with nmemb. Do not assume that the header line is zero terminated! The
   pointer named userdata is the one you set with the CURLOPT_WRITEHEADER
   option. The callback function must return the number of bytes actually taken
   care of. If that amount differs from the amount passed to your function,
   it'll signal an error to the library. This will abort the transfer and
   return CURL_WRITE_ERROR. */
static size_t curl_header_cback (void *ptr, size_t size, size_t nmemb,
                                 void *userdata)
{
  httpsrc_prc_t *p_prc = userdata;
  assert (NULL != p_prc);
  char *p_info = tiz_mem_calloc (1, (size * nmemb) + 1);
  memcpy (p_info, ptr, size * nmemb);
  p_info[(size * nmemb) - 2] = '\000'; /* -2 to remove newline */
  TIZ_TRACE (handleOf (p_prc), "libcurl : [%s]", p_info);
  tiz_mem_free (p_info);
  return size * nmemb;
}

/* This function gets called by libcurl as soon as there is data received that
   needs to be saved. The size of the data pointed to by ptr is size multiplied
   with nmemb, it will not be zero terminated. Return the number of bytes
   actually taken care of. If that amount differs from the amount passed to
   your function, it'll signal an error to the library. This will abort the
   transfer and return CURLE_WRITE_ERROR.  */
static size_t curl_write_cback (void *ptr, size_t size, size_t nmemb,
                                void *userdata)
{
  httpsrc_prc_t *p_prc = userdata;
  size_t nbytes = size * nmemb;
  assert (NULL != p_prc);
  TIZ_TRACE (handleOf (p_prc), "size [%d] nmemb [%d] sockfd [%d]", size, nmemb,
             p_prc->sockfd_);

  if (nbytes > 0)
    {
      OMX_BUFFERHEADERTYPE *p_out = NULL;
      /* At this point, we are not interested anymore in the write event, so
         modify the io watcher to register interest in the read event only */
      stop_io_watcher (p_prc);
      tiz_event_io_set (p_prc->p_ev_io_, p_prc->sockfd_, TIZ_EVENT_READ, true);

      p_out = buffer_needed (p_prc);
      if (NULL != p_out)
        {
          memcpy (p_out->pBuffer + p_out->nOffset, ptr, nbytes);
          p_out->nFilledLen = nbytes;
          release_buffer (p_prc);
        }
    }

  return nbytes;
}

#ifdef _DEBUG
/* Pass a pointer to a function that matches the following prototype: int
   curl_debug_callback (CURL *, curl_infotype, char *, size_t, void *);
   CURLOPT_DEBUGFUNCTION replaces the standard debug function used when
   CURLOPT_VERBOSE is in effect. This callback receives debug information, as
   specified with the curl_infotype argument. This function must return 0. The
   data pointed to by the char * passed to this function WILL NOT be zero
   terminated, but will be exactly of the size as told by the size_t
   argument.  */
static size_t curl_debug_cback (CURL *, curl_infotype type, char *buf,
                                size_t nbytes, void *userdata)
{
  if (CURLINFO_TEXT == type || CURLINFO_HEADER_IN == type || CURLINFO_HEADER_OUT
                                                             == type)
    {
      httpsrc_prc_t *p_prc = userdata;
      char *p_info = tiz_mem_calloc (1, nbytes + 1);
      memcpy (p_info, buf, nbytes);
      TIZ_TRACE (handleOf (p_prc), "libcurl : [%s]", p_info);
      tiz_mem_free (p_info);
    }
  return 0;
}
#endif

/* The curl_multi_socket_action(3) function informs the application
   about updates in the socket (file descriptor) status by doing none, one, or
   multiple calls to the curl_socket_callback given in the param argument. They
   update the status with changes since the previous time a
   curl_multi_socket(3) function was called. If the given callback pointer is
   NULL, no callback will be called. Set the callback's userp argument with
   CURLMOPT_SOCKETDATA. See curl_multi_socket(3) for more callback details. */

/* The callback MUST return 0. */

/* The easy argument is a pointer to the easy handle that deals with this
   particular socket. Note that a single handle may work with several sockets
   simultaneously. */

/* The s argument is the actual socket value as you use it within your
   system. */

/* The action argument to the callback has one of five values: */
/* CURL_POLL_NONE (0) */
/* register, not interested in readiness (yet) */
/* CURL_POLL_IN (1) */
/* register, interested in read readiness */
/* CURL_POLL_OUT (2) */
/* register, interested in write readiness */
/* CURL_POLL_INOUT (3) */
/* register, interested in both read and write readiness */
/* CURL_POLL_REMOVE (4) */
/* unregister */

/* The socketp argument is a private pointer you have previously set with
   curl_multi_assign(3) to be associated with the s socket. If no pointer has
   been set, socketp will be NULL. This argument is of course a service to
   applications that want to keep certain data or structs that are strictly
   associated to the given socket. */

/* The userp argument is a private pointer you have previously set with
   curl_multi_setopt(3) and the CURLMOPT_SOCKETDATA option.  */

static int curl_socket_cback (CURL *easy, curl_socket_t s, int action,
                              void *userp, void *socketp)
{
  httpsrc_prc_t *p_prc = userp;
  assert (NULL != p_prc);
  TIZ_DEBUG (
      handleOf (p_prc),
      "socket [%d] action [%d] (1 READ, 2 WRITE, 3 READ/WRITE, 4 REMOVE)", s,
      action);
  if (-1 == p_prc->sockfd_)
    {
      p_prc->sockfd_ = s;
      tiz_event_io_set (p_prc->p_ev_io_, s, TIZ_EVENT_READ_OR_WRITE, true);
      (void)start_io_watcher (p_prc);
    }
  return 0;
}

/* This function will then be called when the timeout value changes. The
   timeout value is at what latest time the application should call one of the
   "performing" functions of the multi interface (curl_multi_socket_action(3)
   and curl_multi_perform(3)) - to allow libcurl to keep timeouts and retries
   etc to work. A timeout value of -1 means that there is no timeout at all,
   and 0 means that the timeout is already reached. Libcurl attempts to limit
   calling this only when the fixed future timeout time actually changes. See
   also CURLMOPT_TIMERDATA. The callback should return 0 on success, and -1 on
   error. This callback can be used instead of, or in addition to,
   curl_multi_timeout(3). (Added in 7.16.0) */
static int curl_timer_cback (CURLM *multi, long timeout_ms, void *userp)
{
  httpsrc_prc_t *p_prc = userp;
  assert (NULL != p_prc);

  TIZ_DEBUG (handleOf (p_prc), "timeout_ms : %d", timeout_ms);

  if (timeout_ms < 0)
    {
      stop_timer_watcher (p_prc);
      p_prc->curl_timeout_ = 0;
    }
  else
    {
      p_prc->curl_timeout_ = ((double)timeout_ms / (double)1000);
      (void)start_timer_watcher (p_prc);
    }
  return 0;
}

static void destroy_curl_resources (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  curl_slist_free_all (ap_prc->p_http_ok_aliases_);
  ap_prc->p_http_ok_aliases_ = NULL;
  curl_slist_free_all (ap_prc->p_http_headers_);
  ap_prc->p_http_headers_ = NULL;
  curl_multi_cleanup (ap_prc->p_curl_multi_);
  ap_prc->p_curl_multi_ = NULL;
  curl_easy_cleanup (ap_prc->p_curl_);
  ap_prc->p_curl_ = NULL;
}

static OMX_ERRORTYPE allocate_curl_global_resources (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorInsufficientResources;
  bail_on_curl_error (curl_global_init (CURL_GLOBAL_ALL));
  /* All well */
  rc = OMX_ErrorNone;
end:
  return rc;
}

static OMX_ERRORTYPE allocate_curl_resources (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorInsufficientResources;

  assert (NULL == ap_prc->p_curl_);
  assert (NULL == ap_prc->p_curl_multi_);

  tiz_check_omx_err (allocate_curl_global_resources (ap_prc));

  TIZ_DEBUG (handleOf (ap_prc), "%s", curl_version ());

  /* Init the curl easy handle */
  tiz_check_null_ret_oom ((ap_prc->p_curl_ = curl_easy_init ()));
  /* Now init the curl multi handle */
  bail_on_oom ((ap_prc->p_curl_multi_ = curl_multi_init ()));
  /* this is to ask libcurl to accept ICY OK headers*/
  bail_on_oom ((ap_prc->p_http_ok_aliases_ = curl_slist_append (
                    ap_prc->p_http_ok_aliases_, "ICY 200 OK")));
  /* and this is to not ask the server for Icy metadata, for now */
  bail_on_oom ((ap_prc->p_http_headers_ = curl_slist_append (
                    ap_prc->p_http_headers_, "Icy-Metadata: 0")));

  /* all ok */
  rc = OMX_ErrorNone;

end:

  if (OMX_ErrorNone != rc)
    {
      /* Clean-up */
      destroy_curl_resources (ap_prc);
    }

  return rc;
}

static OMX_ERRORTYPE start_curl_handles (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorInsufficientResources;

  assert (NULL != ap_prc->p_curl_);
  assert (NULL != ap_prc->p_curl_multi_);

  /* associate the processor with the curl handle */
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_PRIVATE, ap_prc));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_USERAGENT,
                                        ARATELIA_HTTP_SOURCE_COMPONENT_NAME));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_HEADERFUNCTION,
                                        curl_header_cback));
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_WRITEHEADER, ap_prc));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_WRITEFUNCTION,
                                        curl_write_cback));
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_WRITEDATA, ap_prc));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_HTTP200ALIASES,
                                        ap_prc->p_http_ok_aliases_));
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_FOLLOWLOCATION, 1));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_NETRC, 1));
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_MAXREDIRS, 5));
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_FAILONERROR, 1)); /* true */
  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_ERRORBUFFER,
                                        ap_prc->curl_err));
  /* no progress meter */
  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_NOPROGRESS, 1));

  bail_on_curl_error (
      curl_easy_setopt (ap_prc->p_curl_, CURLOPT_CONNECTTIMEOUT, 10));

  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_URL,
                                        ap_prc->p_uri_param_->contentURI));

  bail_on_curl_error (curl_easy_setopt (ap_prc->p_curl_, CURLOPT_HTTPHEADER,
                                        ap_prc->p_http_headers_));

#ifdef _DEBUG
  curl_easy_setopt (ap_prc->p_curl_, CURLOPT_VERBOSE, 1);
  curl_easy_setopt (ap_prc->p_curl_, CURLOPT_DEBUGDATA, ap_prc);
  curl_easy_setopt (ap_prc->p_curl_, CURLOPT_DEBUGFUNCTION, curl_debug_cback);
#endif

  /* Set the socket callback with CURLMOPT_SOCKETFUNCTION */
  bail_on_curl_multi_error (curl_multi_setopt (
      ap_prc->p_curl_multi_, CURLMOPT_SOCKETFUNCTION, curl_socket_cback));
  bail_on_curl_multi_error (
      curl_multi_setopt (ap_prc->p_curl_multi_, CURLMOPT_SOCKETDATA, ap_prc));
  /* Set the timeout callback with CURLMOPT_TIMERFUNCTION, to get to know what
     timeout value to use when waiting for socket activities. */
  bail_on_curl_multi_error (curl_multi_setopt (
      ap_prc->p_curl_multi_, CURLMOPT_TIMERFUNCTION, curl_timer_cback));
  bail_on_curl_multi_error (
      curl_multi_setopt (ap_prc->p_curl_multi_, CURLMOPT_TIMERDATA, ap_prc));
  /* Add the easy handle to the multi */
  bail_on_curl_multi_error (
      curl_multi_add_handle (ap_prc->p_curl_multi_, ap_prc->p_curl_));

  /* all ok */
  rc = OMX_ErrorNone;

end:

  return rc;
}

static inline void delete_uri (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  tiz_mem_free (ap_prc->p_uri_param_);
  ap_prc->p_uri_param_ = NULL;
}

static OMX_ERRORTYPE obtain_uri (httpsrc_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  const long pathname_max = PATH_MAX + NAME_MAX;

  assert (NULL != ap_prc);
  assert (NULL == ap_prc->p_uri_param_);

  ap_prc->p_uri_param_
      = tiz_mem_calloc (1, sizeof(OMX_PARAM_CONTENTURITYPE) + pathname_max + 1);

  if (NULL == ap_prc->p_uri_param_)
    {
      TIZ_ERROR (handleOf (ap_prc),
                 "Error allocating memory for the content uri struct");
      rc = OMX_ErrorInsufficientResources;
    }
  else
    {
      ap_prc->p_uri_param_->nSize = sizeof(OMX_PARAM_CONTENTURITYPE)
                                    + pathname_max + 1;
      ap_prc->p_uri_param_->nVersion.nVersion = OMX_VERSION;

      if (OMX_ErrorNone
          != (rc = tiz_api_GetParameter (
                  tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                  OMX_IndexParamContentURI, ap_prc->p_uri_param_)))
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "[%s] : Error retrieving the URI param from port",
                     tiz_err_to_str (rc));
        }
      else
        {
          TIZ_NOTICE (handleOf (ap_prc), "URI [%s]",
                      ap_prc->p_uri_param_->contentURI);
        }
    }

  return rc;
}

static OMX_ERRORTYPE allocate_events (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  assert (NULL == ap_prc->p_ev_io_);
  assert (NULL == ap_prc->p_ev_timer_);

  /* Allocate the io event */
  tiz_check_omx_err (tiz_event_io_init (&(ap_prc->p_ev_io_), handleOf (ap_prc),
                                        tiz_comp_event_io));
  /* Allocate the timer event */
  tiz_check_omx_err (tiz_event_timer_init (
      &(ap_prc->p_ev_timer_), handleOf (ap_prc), tiz_comp_event_timer, ap_prc));

  return OMX_ErrorNone;
}

static void destroy_events (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);
  tiz_event_io_destroy (ap_prc->p_ev_io_);
  ap_prc->p_ev_io_ = NULL;
  tiz_event_timer_destroy (ap_prc->p_ev_timer_);
  ap_prc->p_ev_timer_ = NULL;
}

static OMX_ERRORTYPE release_buffer (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);

  if (ap_prc->p_outhdr_)
    {
      TIZ_NOTICE (handleOf (ap_prc), "releasing HEADER [%p] nFilledLen [%d]",
                  ap_prc->p_outhdr_, ap_prc->p_outhdr_->nFilledLen);

      tiz_check_omx_err (tiz_krn_release_buffer (
          tiz_get_krn (handleOf (ap_prc)), 0, ap_prc->p_outhdr_));
      ap_prc->p_outhdr_ = NULL;
    }
  return OMX_ErrorNone;
}

static OMX_BUFFERHEADERTYPE *buffer_needed (httpsrc_prc_t *ap_prc)
{
  assert (NULL != ap_prc);

  if (!ap_prc->port_disabled_)
    {
      if (NULL != ap_prc->p_outhdr_)
        {
          return ap_prc->p_outhdr_;
        }
      else
        {
          if (OMX_ErrorNone
              == (tiz_krn_claim_buffer (tiz_get_krn (handleOf (ap_prc)),
                                        ARATELIA_HTTP_SOURCE_PORT_INDEX, 0,
                                        &ap_prc->p_outhdr_)))
            {
              if (NULL != ap_prc->p_outhdr_)
                {
                  TIZ_TRACE (handleOf (ap_prc),
                             "Claimed HEADER [%p]...nFilledLen [%d]",
                             ap_prc->p_outhdr_, ap_prc->p_outhdr_->nFilledLen);
                  return ap_prc->p_outhdr_;
                }
            }
        }
    }
  return NULL;
}

/*
 * httpsrcprc
 */

static void *httpsrc_prc_ctor (void *ap_obj, va_list *app)
{
  httpsrc_prc_t *p_prc
      = super_ctor (typeOf (ap_obj, "httpsrcprc"), ap_obj, app);
  p_prc->p_outhdr_ = NULL;
  p_prc->p_uri_param_ = NULL;
  p_prc->eos_ = false;
  p_prc->port_disabled_ = false;
  p_prc->first_buffer_ = true;
  p_prc->p_ev_io_ = NULL;
  p_prc->sockfd_ = -1;
  p_prc->awaiting_io_ev_ = false;
  p_prc->p_ev_timer_ = NULL;
  p_prc->awaiting_timer_ev_ = false;
  p_prc->curl_timeout_ = 0;
  p_prc->p_curl_ = NULL;
  p_prc->p_curl_multi_ = NULL;
  p_prc->p_http_ok_aliases_ = NULL;
  p_prc->p_http_headers_ = NULL;
  return p_prc;
}

static void *httpsrc_prc_dtor (void *ap_obj)
{
  (void)httpsrc_prc_deallocate_resources (ap_obj);
  return super_dtor (typeOf (ap_obj, "httpsrcprc"), ap_obj);
}

/*
 * from tizsrv class
 */

static OMX_ERRORTYPE httpsrc_prc_allocate_resources (void *ap_obj,
                                                     OMX_U32 a_pid)
{
  httpsrc_prc_t *p_prc = ap_obj;
  assert (NULL != p_prc);
  assert (NULL == p_prc->p_uri_param_);

  tiz_check_omx_err (obtain_uri (p_prc));
  tiz_check_omx_err (allocate_events (p_prc));
  tiz_check_omx_err (allocate_curl_resources (p_prc));

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_deallocate_resources (void *ap_obj)
{
  destroy_events (ap_obj);
  destroy_curl_resources (ap_obj);
  delete_uri (ap_obj);
  curl_global_cleanup ();
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_prepare_to_transfer (void *ap_obj,
                                                      OMX_U32 a_pid)
{
  httpsrc_prc_t *p_prc = ap_obj;
  assert (NULL != ap_obj);

  p_prc->eos_ = false;
  p_prc->first_buffer_ = true;
  p_prc->sockfd_ = -1;
  p_prc->awaiting_io_ev_ = false;
  p_prc->awaiting_timer_ev_ = false;
  p_prc->curl_timeout_ = 0;

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_transfer_and_process (void *ap_prc,
                                                       OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_stop_and_return (void *ap_obj)
{
  stop_io_watcher (ap_obj);
  stop_timer_watcher (ap_obj);
  return release_buffer (ap_obj);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE httpsrc_prc_buffers_ready (const void *ap_prc)
{
  httpsrc_prc_t *p_prc = (httpsrc_prc_t *)ap_prc;
  assert (NULL != p_prc);

  TIZ_TRACE (handleOf (ap_prc), "Received buffer event ");

  if (p_prc->first_buffer_)
    {
      int running_handles = 0;
      tiz_check_omx_err (start_curl_handles (p_prc));
      assert (NULL != p_prc->p_curl_multi_);
      /* Kickstart curl to get one or more callbacks called. */
      on_curl_multi_error_ret_omx_oom (curl_multi_socket_action (
          p_prc->p_curl_multi_, CURL_SOCKET_TIMEOUT, 0, &running_handles));
      TIZ_NOTICE (handleOf (p_prc), "running handles [%d]", running_handles);
      p_prc->first_buffer_ = false;
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_io_ready (void *ap_prc,
                                           tiz_event_io_t *ap_ev_io, int a_fd,
                                           int a_events)
{
  httpsrc_prc_t *p_prc = ap_prc;
  assert (NULL != p_prc);

  if (p_prc->awaiting_io_ev_)
    {
      int running_handles = 0;
      int curl_ev_bitmask = 0;
      TIZ_TRACE (handleOf (ap_prc), "Received io event on fd [%d] events [%d]",
                 a_fd, a_events);
      if (TIZ_EVENT_READ == a_events || TIZ_EVENT_READ_OR_WRITE == a_events)
        {
          curl_ev_bitmask |= CURL_CSELECT_IN;
        }
      if (TIZ_EVENT_WRITE == a_events || TIZ_EVENT_READ_OR_WRITE == a_events)
        {
          curl_ev_bitmask |= CURL_CSELECT_OUT;
        }
      tiz_check_omx_err (stop_io_watcher (ap_prc));
      tiz_check_omx_err (restart_timer_watcher (ap_prc));
      on_curl_multi_error_ret_omx_oom (curl_multi_socket_action (
          p_prc->p_curl_multi_, a_fd, curl_ev_bitmask, &running_handles));
      TIZ_NOTICE (handleOf (p_prc),
                  "Received timer event : running handles [%d]",
                  running_handles);
      tiz_check_omx_err (start_io_watcher (ap_prc));
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_timer_ready (void *ap_prc,
                                              tiz_event_timer_t *ap_ev_timer,
                                              void *ap_arg)
{
  httpsrc_prc_t *p_prc = ap_prc;
  assert (NULL != p_prc);

  if (p_prc->awaiting_timer_ev_)
    {
      int running_handles = 0;
      tiz_check_omx_err (restart_timer_watcher (ap_arg));
      on_curl_multi_error_ret_omx_oom (curl_multi_socket_action (
          p_prc->p_curl_multi_, CURL_SOCKET_TIMEOUT, 0, &running_handles));
      TIZ_NOTICE (handleOf (p_prc), "running handles [%d]", running_handles);
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_pause (const void *ap_obj)
{
  httpsrc_prc_t *p_prc = (httpsrc_prc_t *)ap_obj;
  assert (NULL != p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_resume (const void *ap_obj)
{
  httpsrc_prc_t *p_prc = (httpsrc_prc_t *)ap_obj;
  assert (NULL != p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE httpsrc_prc_port_flush (const void *ap_obj,
                                             OMX_U32 TIZ_UNUSED (a_pid))
{
  httpsrc_prc_t *p_prc = (httpsrc_prc_t *)ap_obj;
  assert (NULL != p_prc);
  return release_buffer (p_prc);
}

static OMX_ERRORTYPE httpsrc_prc_port_disable (const void *ap_obj,
                                               OMX_U32 TIZ_UNUSED (a_pid))
{
  httpsrc_prc_t *p_prc = (httpsrc_prc_t *)ap_obj;
  assert (NULL != p_prc);
  /* Release any buffers held  */
  return release_buffer ((httpsrc_prc_t *)ap_obj);
}

static OMX_ERRORTYPE httpsrc_prc_port_enable (const void *ap_obj,
                                              OMX_U32 TIZ_UNUSED (a_pid))
{
  /* TODO */
  return OMX_ErrorNone;
}

/*
 * httpsrc_prc_class
 */

static void *httpsrc_prc_class_ctor (void *ap_obj, va_list *app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_obj, "httpsrcprc_class"), ap_obj, app);
}

/*
 * initialization
 */

void *httpsrc_prc_class_init (void *ap_tos, void *ap_hdl)
{
  void *tizprc = tiz_get_type (ap_hdl, "tizprc");
  void *httpsrcprc_class = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (classOf (tizprc), "httpsrcprc_class", classOf (tizprc),
       sizeof(httpsrc_prc_class_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, httpsrc_prc_class_ctor,
       /* TIZ_CLASS_COMMENT: stop value*/
       0);
  return httpsrcprc_class;
}

void *httpsrc_prc_init (void *ap_tos, void *ap_hdl)
{
  void *tizprc = tiz_get_type (ap_hdl, "tizprc");
  void *httpsrcprc_class = tiz_get_type (ap_hdl, "httpsrcprc_class");
  TIZ_LOG_CLASS (httpsrcprc_class);
  void *httpsrcprc = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (httpsrcprc_class, "httpsrcprc", tizprc, sizeof(httpsrc_prc_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, httpsrc_prc_ctor,
       /* TIZ_CLASS_COMMENT: class destructor */
       dtor, httpsrc_prc_dtor,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_allocate_resources, httpsrc_prc_allocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_deallocate_resources, httpsrc_prc_deallocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_prepare_to_transfer, httpsrc_prc_prepare_to_transfer,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_transfer_and_process, httpsrc_prc_transfer_and_process,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_stop_and_return, httpsrc_prc_stop_and_return,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_buffers_ready, httpsrc_prc_buffers_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_io_ready, httpsrc_prc_io_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_timer_ready, httpsrc_prc_timer_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_pause, httpsrc_prc_pause,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_resume, httpsrc_prc_resume,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_flush, httpsrc_prc_port_flush,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_disable, httpsrc_prc_port_disable,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_enable, httpsrc_prc_port_enable,
       /* TIZ_CLASS_COMMENT: stop value */
       0);

  return httpsrcprc;
}