/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/httpcli/httpcli.h"

#include <string.h>

#include "src/core/endpoint/endpoint.h"
#include "src/core/endpoint/resolve_address.h"
#include "src/core/endpoint/tcp_client.h"
#include "src/core/httpcli/format_request.h"
#include "src/core/httpcli/httpcli_security_context.h"
#include "src/core/httpcli/parser.h"
#include "src/core/security/security_context.h"
#include "src/core/security/google_root_certs.h"
#include "src/core/security/secure_transport_setup.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string.h>

typedef struct {
  gpr_slice request_text;
  grpc_httpcli_parser parser;
  grpc_resolved_addresses *addresses;
  size_t next_address;
  grpc_endpoint *ep;
  grpc_em *em;
  char *host;
  gpr_timespec deadline;
  int have_read_byte;
  int use_ssl;
  grpc_httpcli_response_cb on_response;
  void *user_data;
} internal_request;

static void next_address(internal_request *req);

static void finish(internal_request *req, int success) {
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  req->on_response(req->user_data, success ? &req->parser.r : NULL);
  grpc_httpcli_parser_destroy(&req->parser);
  if (req->addresses != NULL) {
    grpc_resolved_addresses_destroy(req->addresses);
  }
  if (req->ep != NULL) {
    grpc_endpoint_destroy(req->ep);
  }
  gpr_slice_unref(req->request_text);
  gpr_free(req->host);
  gpr_free(req);
}

static void on_read(void *user_data, gpr_slice *slices, size_t nslices,
                    grpc_endpoint_cb_status status) {
  internal_request *req = user_data;
  size_t i;

  gpr_log(GPR_DEBUG, "%s nslices=%d status=%d", __FUNCTION__, nslices, status);

  for (i = 0; i < nslices; i++) {
    if (GPR_SLICE_LENGTH(slices[i])) {
      req->have_read_byte = 1;
      if (!grpc_httpcli_parser_parse(&req->parser, slices[i])) {
        finish(req, 0);
        goto done;
      }
    }
  }

  switch (status) {
    case GRPC_ENDPOINT_CB_OK:
      grpc_endpoint_notify_on_read(req->ep, on_read, req, gpr_inf_future);
      break;
    case GRPC_ENDPOINT_CB_EOF:
    case GRPC_ENDPOINT_CB_ERROR:
    case GRPC_ENDPOINT_CB_SHUTDOWN:
    case GRPC_ENDPOINT_CB_TIMED_OUT:
      if (!req->have_read_byte) {
        next_address(req);
      } else {
        finish(req, grpc_httpcli_parser_eof(&req->parser));
      }
      break;
  }

done:
  for (i = 0; i < nslices; i++) {
    gpr_slice_unref(slices[i]);
  }
}

static void on_written(internal_request *req) {
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  grpc_endpoint_notify_on_read(req->ep, on_read, req, gpr_inf_future);
}

static void done_write(void *arg, grpc_endpoint_cb_status status) {
  internal_request *req = arg;
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  switch (status) {
    case GRPC_ENDPOINT_CB_OK:
      on_written(req);
      break;
    case GRPC_ENDPOINT_CB_EOF:
    case GRPC_ENDPOINT_CB_SHUTDOWN:
    case GRPC_ENDPOINT_CB_ERROR:
    case GRPC_ENDPOINT_CB_TIMED_OUT:
      next_address(req);
      break;
  }
}

static void start_write(internal_request *req) {
  gpr_slice_ref(req->request_text);
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  switch (grpc_endpoint_write(req->ep, &req->request_text, 1, done_write, req,
                              gpr_inf_future)) {
    case GRPC_ENDPOINT_WRITE_DONE:
      on_written(req);
      break;
    case GRPC_ENDPOINT_WRITE_PENDING:
      break;
    case GRPC_ENDPOINT_WRITE_ERROR:
      finish(req, 0);
      break;
  }
}

static void on_secure_transport_setup_done(void *rp,
                                           grpc_security_status status,
                                           grpc_endpoint *secure_endpoint) {
  internal_request *req = rp;
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  if (status != GRPC_SECURITY_OK) {
    gpr_log(GPR_ERROR, "Secure transport setup failed with error %d.", status);
    finish(req, 0);
  } else {
    req->ep = secure_endpoint;
    start_write(req);
  }
}

static void on_connected(void *arg, grpc_endpoint *tcp) {
  internal_request *req = arg;

  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  if (!tcp) {
    next_address(req);
    return;
  }
  req->ep = tcp;
  if (req->use_ssl) {
    grpc_channel_security_context *ctx = NULL;
    GPR_ASSERT(grpc_httpcli_ssl_channel_security_context_create(
                   grpc_google_root_certs, grpc_google_root_certs_size,
                   req->host, &ctx) == GRPC_SECURITY_OK);
    grpc_setup_secure_transport(&ctx->base, tcp, on_secure_transport_setup_done,
                                req);
    grpc_security_context_unref(&ctx->base);
  } else {
    start_write(req);
  }
}

static void next_address(internal_request *req) {
  grpc_resolved_address *addr;
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  if (req->next_address == req->addresses->naddrs) {
    finish(req, 0);
    return;
  }
  addr = &req->addresses->addrs[req->next_address++];
  grpc_tcp_client_connect(on_connected, req, req->em,
                          (struct sockaddr *)&addr->addr, addr->len,
                          req->deadline);
}

static void on_resolved(void *arg, grpc_resolved_addresses *addresses) {
  internal_request *req = arg;
  gpr_log(GPR_DEBUG, "%s", __FUNCTION__);
  if (!addresses) {
    finish(req, 0);
  }
  req->addresses = addresses;
  req->next_address = 0;
  next_address(req);
}

void grpc_httpcli_get(const grpc_httpcli_request *request,
                      gpr_timespec deadline, grpc_em *em,
                      grpc_httpcli_response_cb on_response, void *user_data) {
  internal_request *req = gpr_malloc(sizeof(internal_request));
  memset(req, 0, sizeof(*req));
  req->request_text = grpc_httpcli_format_get_request(request);
  grpc_httpcli_parser_init(&req->parser);
  req->on_response = on_response;
  req->user_data = user_data;
  req->em = em;
  req->deadline = deadline;
  req->use_ssl = request->use_ssl;
  if (req->use_ssl) {
    req->host = gpr_strdup(request->host);
  }

  grpc_resolve_address(request->host, req->use_ssl ? "https" : "http",
                       on_resolved, req);
}

void grpc_httpcli_post(const grpc_httpcli_request *request,
                       const char *body_bytes, size_t body_size,
                       gpr_timespec deadline, grpc_em *em,
                       grpc_httpcli_response_cb on_response, void *user_data) {
  internal_request *req = gpr_malloc(sizeof(internal_request));
  memset(req, 0, sizeof(*req));
  req->request_text =
      grpc_httpcli_format_post_request(request, body_bytes, body_size);
  grpc_httpcli_parser_init(&req->parser);
  req->on_response = on_response;
  req->user_data = user_data;
  req->em = em;
  req->deadline = deadline;
  req->use_ssl = request->use_ssl;
  if (req->use_ssl) {
    req->host = gpr_strdup(request->host);
  }

  grpc_resolve_address(request->host, req->use_ssl ? "https" : "http",
                       on_resolved, req);
}
