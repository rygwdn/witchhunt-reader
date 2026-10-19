#pragma once

#include <cstdio>
#include <string>
#include <string_view>

#include <WiFiClient.h>

inline std::string httpChunkedResponse(const std::string_view body, const size_t chunkSize = 37) {
  std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  for (size_t offset = 0; offset < body.size();) {
    const size_t length = body.size() - offset < chunkSize ? body.size() - offset : chunkSize;
    char size[24];
    snprintf(size, sizeof(size), "%zx\r\n", length);
    response += size;
    response.append(body.data() + offset, length);
    response += "\r\n";
    offset += length;
  }
  response += "0\r\n\r\n";
  return response;
}

inline std::string httpRequestBody(const std::string& request) {
  const size_t delimiter = request.find("\r\n\r\n");
  return delimiter == std::string::npos ? std::string() : request.substr(delimiter + 4);
}

inline std::string jsonQuote(const std::string_view value) {
  std::string encoded;
  encoded.reserve(value.size() + 2);
  encoded.push_back('"');
  for (const unsigned char c : value) {
    switch (c) {
      case '"':
      case '\\':
        encoded.push_back('\\');
        encoded.push_back(static_cast<char>(c));
        break;
      case '\n':
        encoded += "\\n";
        break;
      case '\r':
        encoded += "\\r";
        break;
      case '\t':
        encoded += "\\t";
        break;
      default:
        encoded.push_back(static_cast<char>(c));
        break;
    }
  }
  encoded.push_back('"');
  return encoded;
}
