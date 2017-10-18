#include "test/integration/http_integration.h"

#include "gtest/gtest.h"

namespace Envoy {

class LuaIntegrationTest : public HttpIntegrationTest,
                           public testing::TestWithParam<Network::Address::IpVersion> {
public:
  LuaIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();
    fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP1, version_));
    ports_.push_back(fake_upstreams_.back()->localAddress()->ip()->port());
  }

  void initializeFilter(const std::string& filter_config) {
    config_helper_.addFilter(filter_config);

    config_helper_.addConfigModifier([](envoy::api::v2::Bootstrap& bootstrap) {
      auto* lua_cluster = bootstrap.mutable_static_resources()->add_clusters();
      lua_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      lua_cluster->set_name("lua_cluster");
    });

    initialize();
  }

  void cleanup() {
    codec_client_->close();
    if (fake_lua_connection_ != nullptr) {
      fake_lua_connection_->close();
      fake_lua_connection_->waitForDisconnect();
    }
    if (fake_upstream_connection_ != nullptr) {
      fake_upstream_connection_->close();
      fake_upstream_connection_->waitForDisconnect();
    }
  }

  FakeHttpConnectionPtr fake_lua_connection_;
  FakeStreamPtr lua_request_;
};

INSTANTIATE_TEST_CASE_P(IpVersions, LuaIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

// Basic request and response.
TEST_P(LuaIntegrationTest, RequestAndResponse) {
  const std::string FILTER_AND_CODE =
      R"EOF(
name: envoy.lua
config:
  deprecated_v1: true
  value:
    inline_code: |
      function envoy_on_request(request_handle)
        request_handle:logTrace("log test")
        request_handle:logDebug("log test")
        request_handle:logInfo("log test")
        request_handle:logWarn("log test")
        request_handle:logErr("log test")
        request_handle:logCritical("log test")

        request_handle:headers():add("request_body_size", request_handle:body():byteSize())
      end

      function envoy_on_response(response_handle)
        response_handle:headers():add("response_body_size", response_handle:body():byteSize())
        response_handle:headers():remove("foo")
      end
)EOF";

  initializeFilter(FILTER_AND_CODE);
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestHeaderMapImpl request_headers{{":method", "POST"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "host"},
                                          {"x-forwarded-for", "10.0.0.1"}};

  Http::StreamEncoder& encoder = codec_client_->startRequest(request_headers, *response_);
  Buffer::OwnedImpl request_data1("hello");
  encoder.encodeData(request_data1, false);
  Buffer::OwnedImpl request_data2("world");
  encoder.encodeData(request_data2, true);

  waitForNextUpstreamRequest();
  EXPECT_STREQ("10", upstream_request_->headers()
                         .get(Http::LowerCaseString("request_body_size"))
                         ->value()
                         .c_str());

  Http::TestHeaderMapImpl response_headers{{":status", "200"}, {"foo", "bar"}};
  upstream_request_->encodeHeaders(response_headers, false);
  Buffer::OwnedImpl response_data1("good");
  upstream_request_->encodeData(response_data1, false);
  Buffer::OwnedImpl response_data2("bye");
  upstream_request_->encodeData(response_data2, true);

  response_->waitForEndStream();

  EXPECT_STREQ(
      "7", response_->headers().get(Http::LowerCaseString("response_body_size"))->value().c_str());
  EXPECT_EQ(nullptr, response_->headers().get(Http::LowerCaseString("foo")));

  cleanup();
}

// Upstream call followed by continuation.
TEST_P(LuaIntegrationTest, UpstreamHttpCall) {
  const std::string FILTER_AND_CODE =
      R"EOF(
name: envoy.lua
config:
  deprecated_v1: true
  value:
    inline_code: |
      function envoy_on_request(request_handle)
        local headers, body = request_handle:httpCall(
        "lua_cluster",
        {
          [":method"] = "POST",
          [":path"] = "/",
          [":authority"] = "lua_cluster"
        },
        "hello world",
        5000)

        request_handle:headers():add("upstream_foo", headers["foo"])
        request_handle:headers():add("upstream_body_size", #body)
      end
)EOF";

  initializeFilter(FILTER_AND_CODE);

  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestHeaderMapImpl request_headers{{":method", "GET"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "host"},
                                          {"x-forwarded-for", "10.0.0.1"}};
  codec_client_->makeHeaderOnlyRequest(request_headers, *response_);

  fake_lua_connection_ = fake_upstreams_[1]->waitForHttpConnection(*dispatcher_);
  lua_request_ = fake_lua_connection_->waitForNewStream(*dispatcher_);
  lua_request_->waitForEndStream(*dispatcher_);
  Http::TestHeaderMapImpl response_headers{{":status", "200"}, {"foo", "bar"}};
  lua_request_->encodeHeaders(response_headers, false);
  Buffer::OwnedImpl response_data1("good");
  lua_request_->encodeData(response_data1, true);

  waitForNextUpstreamRequest();
  EXPECT_STREQ(
      "bar",
      upstream_request_->headers().get(Http::LowerCaseString("upstream_foo"))->value().c_str());
  EXPECT_STREQ("4", upstream_request_->headers()
                        .get(Http::LowerCaseString("upstream_body_size"))
                        ->value()
                        .c_str());

  upstream_request_->encodeHeaders(default_response_headers_, true);
  response_->waitForEndStream();

  cleanup();
}

// Upstream call followed by immediate response.
TEST_P(LuaIntegrationTest, UpstreamCallAndRespond) {
  const std::string FILTER_AND_CODE =
      R"EOF(
name: envoy.lua
config:
  deprecated_v1: true
  value:
    inline_code: |
      function envoy_on_request(request_handle)
        local headers, body = request_handle:httpCall(
        "lua_cluster",
        {
          [":method"] = "POST",
          [":path"] = "/",
          [":authority"] = "lua_cluster"
        },
        "hello world",
        5000)

        request_handle:respond(
          {[":status"] = "403",
           ["upstream_foo"] = headers["foo"]},
          "nope")
      end
)EOF";

  initializeFilter(FILTER_AND_CODE);

  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestHeaderMapImpl request_headers{{":method", "GET"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "host"},
                                          {"x-forwarded-for", "10.0.0.1"}};
  codec_client_->makeHeaderOnlyRequest(request_headers, *response_);

  fake_lua_connection_ = fake_upstreams_[1]->waitForHttpConnection(*dispatcher_);
  lua_request_ = fake_lua_connection_->waitForNewStream(*dispatcher_);
  lua_request_->waitForEndStream(*dispatcher_);
  Http::TestHeaderMapImpl response_headers{{":status", "200"}, {"foo", "bar"}};
  lua_request_->encodeHeaders(response_headers, true);

  response_->waitForEndStream();
  cleanup();

  EXPECT_TRUE(response_->complete());
  EXPECT_STREQ("403", response_->headers().Status()->value().c_str());
  EXPECT_EQ("nope", response_->body());
}

} // namespace Envoy
