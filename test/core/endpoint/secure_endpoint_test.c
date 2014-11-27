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

#include "endpoint_tests.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/core/endpoint/secure_endpoint.h"
#include "src/core/endpoint/tcp.h"
#include "src/core/eventmanager/em.h"
#include "src/core/tsi/fake_transport_security.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include "test/core/util/test_config.h"

grpc_em g_em;

static void create_sockets(int sv[2]) {
  int flags;
  GPR_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  flags = fcntl(sv[0], F_GETFL, 0);
  GPR_ASSERT(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) == 0);
  flags = fcntl(sv[1], F_GETFL, 0);
  GPR_ASSERT(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK) == 0);
}

static grpc_endpoint_test_fixture secure_endpoint_create_fixture_tcp_socketpair(
    ssize_t slice_size, gpr_slice *leftover_slices, size_t leftover_nslices) {
  int sv[2];
  tsi_frame_protector *fake_read_protector = tsi_create_fake_protector(NULL);
  tsi_frame_protector *fake_write_protector = tsi_create_fake_protector(NULL);
  grpc_endpoint_test_fixture f;
  grpc_endpoint *tcp_read;
  grpc_endpoint *tcp_write;

  create_sockets(sv);
  grpc_em_init(&g_em);
  tcp_read = grpc_tcp_create_dbg(sv[0], &g_em, slice_size);
  tcp_write = grpc_tcp_create(sv[1], &g_em);

  if (leftover_nslices == 0) {
    f.client_ep =
        grpc_secure_endpoint_create(fake_read_protector, tcp_read, NULL, 0);
  } else {
    int i;
    tsi_result result;
    gpr_uint32 still_pending_size;
    size_t total_buffer_size = 8192;
    size_t buffer_size = total_buffer_size;
    gpr_uint8 *encrypted_buffer = gpr_malloc(buffer_size);
    gpr_uint8 *cur = encrypted_buffer;
    gpr_slice encrypted_leftover;
    for (i = 0; i < leftover_nslices; i++) {
      gpr_slice plain = leftover_slices[i];
      gpr_uint8 *message_bytes = GPR_SLICE_START_PTR(plain);
      size_t message_size = GPR_SLICE_LENGTH(plain);
      while (message_size > 0) {
        gpr_uint32 protected_buffer_size_to_send = buffer_size;
        gpr_uint32 processed_message_size = message_size;
        result = tsi_frame_protector_protect(
            fake_write_protector, message_bytes, &processed_message_size, cur,
            &protected_buffer_size_to_send);
        GPR_ASSERT(result == TSI_OK);
        message_bytes += processed_message_size;
        message_size -= processed_message_size;
        cur += protected_buffer_size_to_send;
        buffer_size -= protected_buffer_size_to_send;

        GPR_ASSERT(buffer_size >= 0);
      }
      gpr_slice_unref(plain);
    }
    do {
      gpr_uint32 protected_buffer_size_to_send = buffer_size;
      result = tsi_frame_protector_protect_flush(fake_write_protector, cur,
                                                 &protected_buffer_size_to_send,
                                                 &still_pending_size);
      GPR_ASSERT(result == TSI_OK);
      cur += protected_buffer_size_to_send;
      buffer_size -= protected_buffer_size_to_send;
      GPR_ASSERT(buffer_size >= 0);
    } while (still_pending_size > 0);
    encrypted_leftover = gpr_slice_from_copied_buffer(
        (const char *)encrypted_buffer, total_buffer_size - buffer_size);
    f.client_ep = grpc_secure_endpoint_create(fake_read_protector, tcp_read,
                                              &encrypted_leftover, 1);
    gpr_slice_unref(encrypted_leftover);
    gpr_free(encrypted_buffer);
  }

  f.server_ep =
      grpc_secure_endpoint_create(fake_write_protector, tcp_write, NULL, 0);
  return f;
}

static grpc_endpoint_test_fixture
secure_endpoint_create_fixture_tcp_socketpair_noleftover(ssize_t slice_size) {
  return secure_endpoint_create_fixture_tcp_socketpair(slice_size, NULL, 0);
}

static grpc_endpoint_test_fixture
secure_endpoint_create_fixture_tcp_socketpair_leftover(ssize_t slice_size) {
  gpr_slice s =
      gpr_slice_from_copied_string("hello world 12345678900987654321");
  grpc_endpoint_test_fixture f;

  f = secure_endpoint_create_fixture_tcp_socketpair(slice_size, &s, 1);
  return f;
}

static void clean_up() { grpc_em_destroy(&g_em); }

static grpc_endpoint_test_config configs[] = {
    {"secure_ep/tcp_socketpair",
     secure_endpoint_create_fixture_tcp_socketpair_noleftover, clean_up},
    {"secure_ep/tcp_socketpair_leftover",
     secure_endpoint_create_fixture_tcp_socketpair_leftover, clean_up},
};

static void verify_leftover(void *user_data, gpr_slice *slices, size_t nslices,
                            grpc_endpoint_cb_status error) {
  gpr_slice s =
      gpr_slice_from_copied_string("hello world 12345678900987654321");

  GPR_ASSERT(error == GRPC_ENDPOINT_CB_OK);
  GPR_ASSERT(nslices == 1);

  GPR_ASSERT(0 == gpr_slice_cmp(s, slices[0]));
  gpr_slice_unref(slices[0]);
  gpr_slice_unref(s);
  *(int *)user_data = 1;
}

static void test_leftover(grpc_endpoint_test_config config,
                          ssize_t slice_size) {
  grpc_endpoint_test_fixture f = config.create_fixture(slice_size);
  int verified = 0;
  gpr_log(GPR_INFO, "Start test left over");

  grpc_endpoint_notify_on_read(f.client_ep, verify_leftover, &verified,
                               gpr_inf_future);
  GPR_ASSERT(verified == 1);

  grpc_endpoint_shutdown(f.client_ep);
  grpc_endpoint_shutdown(f.server_ep);
  grpc_endpoint_destroy(f.client_ep);
  grpc_endpoint_destroy(f.server_ep);
  clean_up();
}

static void destroy_early(void *user_data, gpr_slice *slices, size_t nslices,
                          grpc_endpoint_cb_status error) {
  grpc_endpoint_test_fixture *f = user_data;
  gpr_slice s =
      gpr_slice_from_copied_string("hello world 12345678900987654321");

  GPR_ASSERT(error == GRPC_ENDPOINT_CB_OK);
  GPR_ASSERT(nslices == 1);

  grpc_endpoint_shutdown(f->client_ep);
  grpc_endpoint_destroy(f->client_ep);

  GPR_ASSERT(0 == gpr_slice_cmp(s, slices[0]));
  gpr_slice_unref(slices[0]);
  gpr_slice_unref(s);
}

/* test which destroys the ep before finishing reading */
static void test_destroy_ep_early(grpc_endpoint_test_config config,
                                  ssize_t slice_size) {
  grpc_endpoint_test_fixture f = config.create_fixture(slice_size);
  gpr_log(GPR_INFO, "Start test destroy early");

  grpc_endpoint_notify_on_read(f.client_ep, destroy_early, &f, gpr_inf_future);

  grpc_endpoint_shutdown(f.server_ep);
  grpc_endpoint_destroy(f.server_ep);
  clean_up();
}

int main(int argc, char **argv) {
  grpc_test_init(argc, argv);

  grpc_endpoint_tests(configs[0]);
  test_leftover(configs[1], 1);
  test_destroy_ep_early(configs[1], 1);

  return 0;
}
