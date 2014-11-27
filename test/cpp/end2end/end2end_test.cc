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

#include <thread>
#include "src/cpp/server/rpc_service_method.h"
#include "test/cpp/util/echo.pb.h"
#include "net/util/netutil.h"
#include <grpc++/channel_interface.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/status.h>
#include <gtest/gtest.h>

#include <grpc/grpc.h>
#include <grpc/support/thd.h>

using grpc::cpp::test::util::EchoRequest;
using grpc::cpp::test::util::EchoResponse;
using grpc::cpp::test::util::TestService;

namespace grpc {

class TestServiceImpl : public TestService::Service {
 public:
  Status Echo(const EchoRequest* request, EchoResponse* response) {
    response->set_message(request->message());
    return Status::OK;
  }
};

class End2endTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int port = PickUnusedPortOrDie();
    server_address_ << "localhost:" << port;
    // Setup server
    ServerBuilder builder;
    builder.AddPort(server_address_.str());
    builder.RegisterService(service.service());
    server_ = builder.BuildAndStart();
  }

  void TearDown() override {
    server_->Shutdown();
  }

  std::unique_ptr<Server> server_;
  std::ostringstream server_address_;
  TestServiceImpl service;
};

static void SendRpc(const grpc::string& server_address, int num_rpcs) {
  std::shared_ptr<ChannelInterface> channel =
      CreateChannel(server_address);
  TestService::Stub* stub = TestService::NewStub(channel);
  EchoRequest request;
  EchoResponse response;
  request.set_message("Hello");

  for (int i = 0; i < num_rpcs; ++i) {
    ClientContext context;
    Status s = stub->Echo(&context, request, &response);
    EXPECT_EQ(response.message(), request.message());
    EXPECT_TRUE(s.IsOk());
  }

  delete stub;
}

TEST_F(End2endTest, SimpleRpc) {
  SendRpc(server_address_.str(), 1);
}

TEST_F(End2endTest, MultipleRpcs) {
  vector<std::thread*> threads;
  for (int i = 0; i < 10; ++i) {
    threads.push_back(new std::thread(SendRpc, server_address_.str(), 10));
  }
  for (int i = 0; i < 10; ++i) {
    threads[i]->join();
    delete threads[i];
  }
}

}  // namespace grpc

int main(int argc, char** argv) {
  grpc_init();
  ::testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
