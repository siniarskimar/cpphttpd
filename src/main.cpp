#include <fmt/core.h>
#include <cstdio>
#include <fmt/std.h>
#include <string_view>
#include <optional>
#include <sstream>
#include <iterator>
#include <map>
#include <concepts>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/write.hpp>
#include <boost/url.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include "expected.hpp"

using namespace boost::asio::experimental::awaitable_operators;
namespace asio = boost::asio;
using boost::asio::ip::tcp;

enum HttpResponseStatus {
  HTTP_STATUS_OK = 200,
  HTTP_STATUS_BAD_REQUEST = 400,
  HTTP_STATUS_NOT_FOUND = 404,
  HTTP_STATUS_LENGTH_REQUIRED = 411,
  HTTP_STATUS_URI_TOO_LONG = 414,
  HTTP_STATUS_INTERNAL_SERVER_ERROR = 500
};

constexpr const char* http_status_to_string(HttpResponseStatus status) {
  switch(status) {
  case HTTP_STATUS_OK:
    return "Ok";
  case HTTP_STATUS_BAD_REQUEST:
    return "Bad Request";
  case HTTP_STATUS_NOT_FOUND:
    return "Not Found";
  case HTTP_STATUS_LENGTH_REQUIRED:
    return "Length Required";
  case HTTP_STATUS_URI_TOO_LONG:
    return "URI Too Long";
  case HTTP_STATUS_INTERNAL_SERVER_ERROR:
    return "Internal Server Error";
  }
  return nullptr;
}

struct HttpClient {
  tcp::socket socket;
  bool keepAlive = false;
  unsigned int timeout = 1000;
  unsigned int maxRequests = 1000;
};

struct HttpRequest {
  std::string method;
  boost::urls::static_url<1024> uri;
  std::map<std::string, std::string> headers;
  uint8_t verMajor;
  uint8_t verMinor;
  std::vector<char> body;
};

struct HttpResponse {
  boost::urls::static_url<1024> uri;
  HttpResponseStatus status;
  std::map<std::string, std::string> headers;
  uint8_t verMajor;
  uint8_t verMinor;
  std::vector<char> body;
};

asio::awaitable<rd::expected<std::string, boost::system::error_code>> http_read_headers(
    HttpClient& client) {

  auto executor = co_await asio::this_coro::executor;
  std::string buffer;
  asio::deadline_timer timer(executor, boost::posix_time::milliseconds(client.timeout));

  try {
    std::variant<size_t, std::monostate> result = co_await (
        asio::async_read_until(
            client.socket, asio::dynamic_buffer(buffer), "\r\n\r\n",
            asio::use_awaitable) ||
        timer.async_wait(asio::use_awaitable));

    if(std::holds_alternative<std::monostate>(result)) {
      co_return rd::expected<std::string, boost::system::error_code>(
          rd::unexpect, asio::error::make_error_code(asio::error::operation_aborted));
    }
  } catch(const boost::system::system_error& e) {
    timer.cancel();

    /// EOF is not fail state
    if(e.code() == asio::error::make_error_code(asio::error::eof)) {
      co_return buffer;
    }
    co_return rd::unexpected(e.code());
  }
  timer.cancel();

  co_return buffer;
}

rd::expected<HttpRequest, HttpResponseStatus> http_parse_request(
    const std::string_view& contents) {
  using namespace boost;
  HttpRequest result;
  std::stringstream stream;
  stream << contents;

  stream >> result.method;
  {
    std::string path;
    stream >> path;
    system::result<decltype(result.uri)> r = boost::urls::parse_uri(path);
    if(r.has_error()) {
      auto error = r.error();
      if(error == boost::urls::error::no_space) {
        return rd::unexpected(HTTP_STATUS_URI_TOO_LONG);
      }
      return rd::unexpected(HTTP_STATUS_BAD_REQUEST);
    }
    result.uri = r.value();
  }
  stream.ignore(10, '/');
  std::string line;
  while(!std::getline(stream, line).eof()) {
    if(line.empty()) {
      break;
    }
    auto colonIt = line.find(':');
    auto headerName = line.substr(0, colonIt);

    for(size_t i = 0; i < headerName.size(); i++) {
      headerName[i] = std::tolower(headerName[i]);
    }

    result.headers.emplace(headerName, line.substr(colonIt));
  }
  return result;
}

asio::awaitable<void> send_response(HttpClient& client, const HttpResponse& response) {}

asio::awaitable<HttpResponse> handle_good_request(
    const HttpClient& client, const HttpRequest& request) {

  HttpResponse response;
  if(request.uri.encoded_path() == "/") {
    const std::string_view responseBody = R"(
        <!DOCTYPE html>
        <html>
          <head><title>Hello World</title></head>
          <body><h1>Hello World!</h1></body>
        </html>)";
    response.body = std::vector<char>(responseBody.begin(), responseBody.end());
    response.status = HTTP_STATUS_OK;
    response.uri = request.uri;
    response.headers["content-length"] = std::to_string(responseBody.size());
    response.headers["content-type"] = "text/html; charset=utf-8";
    co_return response;
  }

  co_return response;
}

asio::awaitable<HttpResponse> handle_request(
    const HttpClient& client, std::optional<std::reference_wrapper<HttpRequest>> request,
    HttpResponseStatus status = HTTP_STATUS_OK) {

  HttpResponse response;

  switch(status) {

  case HTTP_STATUS_OK:
    if(request.has_value()) {
      co_return co_await handle_good_request(client, *request);
      break;
    } else {
      status = HTTP_STATUS_BAD_REQUEST;
    }
  case HTTP_STATUS_BAD_REQUEST:
    response.status = status;
    response.headers["connection"] = "close";
    break;

  case HTTP_STATUS_URI_TOO_LONG:
    response.status = status;
    response.headers["connection"] = "close";
    break;
  default:
    response.status = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    response.headers["connection"] = "close";
    break;
  }
  co_return response;
}

asio::awaitable<void> client_handler(HttpClient client) {

  auto& socket = client.socket;
  const auto socketAddress = socket.remote_endpoint().address().to_string();

  while(true) {
    // Determine wvenever keep connection alive (keep-alive)
    auto headers = co_await http_read_headers(client);
    if(!headers.has_value()) {
      auto error = headers.error();

      if(error == asio::error::operation_aborted) {
        fmt::print(stderr, "info: Connection idle-out ({})", socketAddress);
      } else {
        fmt::print(stderr, "error: Error while reading from socket ({})", socketAddress);
      }
      co_return;
    }
    if(headers->size() == 0) {
      client.socket.shutdown(tcp::socket::shutdown_both);
      client.socket.close();
      co_return;
    }

    auto request = http_parse_request(*headers);
    HttpResponse response;
    if(!request.has_value()) {
      response = co_await handle_request(client, std::nullopt, request.error());
    } else {
      response = co_await handle_request(client, *request);
    }
    co_await send_response(client, response);

    bool connectionClose = response.headers.count("connection") &&
                           response.headers.at("connection") == "close";
    bool connectionKeepAlive = response.headers.count("connection") &&
                               response.headers.at("connection") == "keep-alive";

    if(connectionKeepAlive == true) {
      client.keepAlive = true;
    }

    if(client.keepAlive == false || connectionClose) {
      client.socket.shutdown(tcp::socket::shutdown_both);
      client.socket.close();
      co_return;
    }
  }
}

asio::awaitable<void> client_listener() {
  auto executor = co_await asio::this_coro::executor;
  tcp::acceptor acceptor(executor, {tcp::v4(), 8080});

  while(true) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    HttpClient client{std::move(socket), false, 1000};
    asio::co_spawn(executor, client_handler(std::move(client)), asio::detached);
  }
}

int main(const int argc, const char* argv[]) {
  asio::io_context ioContext;

  asio::signal_set sigset(ioContext, SIGINT, SIGTERM);
  sigset.async_wait([&](auto, auto) { ioContext.stop(); });

  asio::co_spawn(ioContext, client_listener(), asio::detached);
  ioContext.run();

  return 0;
}
