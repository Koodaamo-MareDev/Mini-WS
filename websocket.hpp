/*

A simple websocket implementation in C++ by Marcus Erkkilä

Example usage:

// -----------------------------------------------------------------------------------

#include <iostream>
#include <unistd.h>

#include "websocket.hpp"

void on_server_message(WebSocket::Client &client, const WebSocket::Message &msg)
{
	std::cout << "Got " << std::string(msg) << " from client, echoing." << std::endl;
	client.send_message(msg);
}

void on_close_handler(WebSocket::Client &client, const std::string &message)
{
	std::cout << "Websocket closed: " << message << std::endl;
}

void on_client_message(WebSocket::Client &client, const WebSocket::Message &msg)
{
	std::cout << "Got " << std::string(msg) << " from server." << std::endl;
}

int main()
{
	WebSocket::init();
	WebSocket::Server server;
	server.start(8080);

	WebSocket::Client c;
	c.start("127.0.0.1", 8080);
	c.msg_handler = on_client_message;
	c.close_handler = on_close_handler;

	size_t counter = 0;
	while (true)
	{
		server.update();
		c.update();
		if (++counter >= 20)
		{
			counter %= 20;
			std::string msg_str = "abc " + std::to_string(rand() & 0xFF);
			std::vector<uint8_t> msg;
			msg.resize(msg_str.size());
			std::memcpy(msg.data(), msg_str.data(), msg_str.size());
			c.send_message(WebSocket::Message(WebSocket::MessageType::UTF8, msg));
		}
		for (WebSocket::Client &client : server.clients)
		{
			if (!client.msg_handler)
				client.msg_handler = on_server_message;
		}
		usleep(50000);
	}
	return 0;
}

// -----------------------------------------------------------------------------------

*/

#pragma once

#include <string>
#include <memory>
#include <array>
#include <algorithm>
#include <vector>
#include <map>
#include <cstring>
#include <sstream>
#include <fstream>
#include <functional>
#include <chrono>
#include <cstdint>

#ifdef _WIN32

// Windows
#pragma comment(lib, "ws2_32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#define IS_VALIDSOCK(s) ((s) != INVALID_SOCKET)

#else

// POSIX
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>

#define IS_VALIDSOCK(s) ((s) >= 0)

#ifndef NO_WS2_DEFS 

#define SOCKET_ERROR (-1)
#define SOCKET int
#define INVALID_SOCKET (-1)
#define ioctlsocket ioctl
#define closesocket close

#endif // NO_WS2_DEFS

#ifndef NO_ENDIAN_DEFS
#include <endian.h>
#define htonll htobe64
#define ntohll be64toh
#endif // NO_ENDIAN_DEFS

#endif // _WIN32

namespace WebSocket
{

	// Provided because why would you want a full cryptography library for this?
	class SHA1
	{
	public:
		SHA1()
		{
			h_[0] = 0x67452301;
			h_[1] = 0xEFCDAB89;
			h_[2] = 0x98BADCFE;
			h_[3] = 0x10325476;
			h_[4] = 0xC3D2E1F0;
		}

		std::array<uint8_t, 20> digest(const std::string &input)
		{
			// Process complete 64-byte blocks.
			size_t i = 0;
			for (; i + 64 <= input.size(); i += 64)
				process(reinterpret_cast<const uint8_t *>(input.data() + i));

			// Build final block(s).
			uint8_t block[128] = {};
			size_t remaining = input.size() - i;
			std::memcpy(block, input.data() + i, remaining);

			block[remaining] = 0x80;

			// Message length in bits, big-endian.
			uint64_t bits = static_cast<uint64_t>(input.size()) * 8;

			size_t final_size = (remaining < 56) ? 64 : 128;

			for (int j = 0; j < 8; ++j)
				block[final_size - 1 - j] =
					static_cast<uint8_t>(bits >> (j * 8));

			process(block);

			if (final_size == 128)
				process(block + 64);

			std::array<uint8_t, 20> out;

			for (int i = 0; i < 5; ++i)
			{
				out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
				out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
				out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
				out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
			}

			return out;
		}

	private:
		uint32_t h_[5];

		static uint32_t rol(uint32_t x, unsigned n)
		{
			return (x << n) | (x >> (32 - n));
		}

		static uint32_t load_be32(const uint8_t *p)
		{
			return (uint32_t(p[0]) << 24) |
				   (uint32_t(p[1]) << 16) |
				   (uint32_t(p[2]) << 8) |
				   uint32_t(p[3]);
		}

		void process(const uint8_t *block)
		{
			uint32_t w[80];

			for (int i = 0; i < 16; ++i)
				w[i] = load_be32(block + i * 4);

			for (int i = 16; i < 80; ++i)
				w[i] = rol(w[i - 3] ^ w[i - 8] ^
							   w[i - 14] ^ w[i - 16],
						   1);

			uint32_t a = h_[0];
			uint32_t b = h_[1];
			uint32_t c = h_[2];
			uint32_t d = h_[3];
			uint32_t e = h_[4];

			for (int i = 0; i < 80; ++i)
			{
				uint32_t f, k;

				if (i < 20)
				{
					f = (b & c) | (~b & d);
					k = 0x5A827999;
				}
				else if (i < 40)
				{
					f = b ^ c ^ d;
					k = 0x6ED9EBA1;
				}
				else if (i < 60)
				{
					f = (b & c) | (b & d) | (c & d);
					k = 0x8F1BBCDC;
				}
				else
				{
					f = b ^ c ^ d;
					k = 0xCA62C1D6;
				}

				uint32_t temp = rol(a, 5) + f + e + k + w[i];

				e = d;
				d = c;
				c = rol(b, 30);
				b = a;
				a = temp;
			}

			h_[0] += a;
			h_[1] += b;
			h_[2] += c;
			h_[3] += d;
			h_[4] += e;
		}
	};

	inline static std::string base64_encode(const uint8_t *data, size_t len)
	{
		static constexpr char table[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz"
			"0123456789+/";

		std::string out;
		out.reserve((len + 2) / 3 * 4);

		for (size_t i = 0; i < len; i += 3)
		{
			uint32_t n = uint32_t(data[i]) << 16;

			if (i + 1 < len)
				n |= uint32_t(data[i + 1]) << 8;

			if (i + 2 < len)
				n |= uint32_t(data[i + 2]);

			out += table[(n >> 18) & 0x3F];
			out += table[(n >> 12) & 0x3F];

			if (i + 1 < len)
				out += table[(n >> 6) & 0x3F];
			else
				out += '=';

			if (i + 2 < len)
				out += table[n & 0x3F];
			else
				out += '=';
		}

		return out;
	}

	template <size_t N>
	inline static std::string base64_encode(const std::array<uint8_t, N> &data)
	{
		return base64_encode(data.data(), data.size());
	}

	enum class ClientState
	{
		CLOSED,
		HANDSHAKE,
		ACTIVE,
		CLOSING
	};

	enum class MessageType
	{
		NONE,
		UTF8,
		BINARY,
	};

	struct Message
	{
		MessageType type = MessageType::NONE;
		std::vector<uint8_t> buffer{};

		Message() {}

		Message(MessageType type, const std::vector<uint8_t> &data) : type(type), buffer(data) {}

		// Appends bytes to the message buffer with an optional 4-byte XOR mask - if the mask is null, the data is appended as is.
		void append(const void *data, size_t count, const uint8_t *mask = nullptr);

		// Appends bytes to the message buffer with an optional 4-byte XOR mask - if the mask is null, the data is appended as is.
		void append(std::vector<uint8_t> data, const uint8_t *mask = nullptr);

		// Reads N number of bytes to an output buffer and removes it from the internal buffer.
		// Returns true on success, i.e. the requested number of bytes could be read, false otherwise.
		bool read(void *output, size_t count);

		// Clears the message buffer.
		void clear();

		operator std::string() const
		{
			if (type == MessageType::NONE)
				return "";

			return std::string(reinterpret_cast<const char *>(buffer.data()), buffer.size());
		}
	};

	struct HTTPMessage
	{
		// Key-value map where the key = header name (in lowercase), and value is the value (as-is)
		std::map<std::string, std::string> headers;
	};

	// Boilerplate struct for managing per-client user data.
	struct UserData
	{
		virtual ~UserData() = default;
	};

	class Client
	{
	private:
		SOCKET sock = INVALID_SOCKET;

		// HTTP Handshake
		std::string expect_accept_str = "";
		std::array<uint8_t, 65536> http_buffer;
		size_t write_head = 0;

		// Websocket
		bool use_nonce = false;
		std::vector<uint8_t> wsbuffer;
		Message msg{};

		// Timeout
		uint32_t timeout = 0;
		std::chrono::steady_clock::time_point last_ping = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point last_pong = std::chrono::steady_clock::now();

		void update_handshake();
		void update_websocket();
		void ping();
		void pong(uint8_t *data, size_t count);
		void lookup_host(const std::string &host, in_addr *out_address);

	public:
		ClientState state = ClientState::CLOSED;

		std::function<void(Client &, const Message &)> msg_handler = nullptr;
		std::function<void(Client &, const std::string &)> close_handler = nullptr;

		UserData *user_data = nullptr;

		Client(SOCKET sock = INVALID_SOCKET);

		void send_message(const Message &message);
		void start(const std::string &server_ip, uint16_t server_port);
		void update();
		void stop();
		void set_timeout(uint32_t seconds);
	};

	class Server
	{
	private:
		uint32_t timeout = 0;
		SOCKET server_fd = INVALID_SOCKET;
		sockaddr_in address;

	public:
		std::vector<Client> clients;

		Server();
		~Server();

		void start(uint16_t port);
		void update();
		void stop();
		void set_timeout(uint32_t seconds);
	};

	// Do not call this directly.
	void _deinit();

	// Use this to initialize the WebSocket library.
	void init();

} // namespace WebSocket

#ifdef WEBSOCKET_HEADER_ONLY
#include "websocket_impl.hpp"
#endif // WEBSOCKET_HEADER_ONLY