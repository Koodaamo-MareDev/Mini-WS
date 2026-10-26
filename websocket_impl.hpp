#include "websocket.hpp"

namespace WebSocket
{

	inline void Message::append(const void *data, size_t count, const uint8_t *mask)
	{
		size_t orig_size = buffer.size();

		buffer.resize(orig_size + count);

		std::memcpy(&buffer[orig_size], data, count);
		if (mask)
		{
			for (size_t i = orig_size, j = 0; i < buffer.size(); i++, j++)
				buffer[i] ^= mask[j & 3];
		}
	}

	inline void Message::append(std::vector<uint8_t> data, const uint8_t *mask)
	{
		append(&data[0], data.size(), mask);
	}

	inline bool Message::read(void *output, size_t count)
	{
		if (buffer.size() >= count)
		{
			std::memcpy(output, buffer.data(), count);
			buffer.erase(buffer.begin(), buffer.begin() + count);
			return true;
		}

		return false;
	}

	inline void Message::clear()
	{
		buffer.clear();
	}

	inline Client::Client(SOCKET sock) : sock(sock)
	{
	}

	inline void Client::lookup_host(const std::string &host, in_addr *out_address)
	{
		addrinfo hints, *res, *result;
		int errcode;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags |= AI_CANONNAME;

		errcode = getaddrinfo(host.c_str(), NULL, &hints, &result);
		if (errcode != 0)
		{
			throw std::runtime_error("Failed to resolve address.");
		}
		res = result;
		while (res)
		{
			if (res->ai_family == AF_INET)
			{
				std::memcpy(out_address, &((struct sockaddr_in *)res->ai_addr)->sin_addr, sizeof(in_addr));
				return; // Success
			}

			res = res->ai_next;
		}

		freeaddrinfo(result);
		throw std::runtime_error("Failed to resolve address.");
	}

	inline void Client::send_message(const Message &message)
	{
		if (message.type == MessageType::NONE)
			return;
		std::vector<uint8_t> send_buffer;
		size_t send_header_len = 2 + (use_nonce ? 4 : 0);
		if (message.buffer.size() > 125)
		{
			if (message.buffer.size() < 65536)
				send_header_len += 2;
			else
				send_header_len += 8;
		}
		send_buffer.resize(send_header_len + message.buffer.size());
		std::memcpy(&send_buffer[send_header_len], &message.buffer[0], message.buffer.size());
		send_buffer[0] = (message.type == MessageType::UTF8 ? 0x01 : 0x02) | 0x80; // 0x80 = Final frame

		if (message.buffer.size() > 125)
		{
			if (message.buffer.size() < 65536)
			{
				uint16_t data_len = htons(message.buffer.size());
				// 126 = The size is provided in the next 2 bytes
				send_buffer[1] = 126;
				std::memcpy(&send_buffer[2], &data_len, 2);
			}
			else
			{
				uint64_t data_len = htonll(message.buffer.size()) & 0x7FFFFFFFFFFFFFFF;

				// 127 = The size is provided in the next 8 bytes
				send_buffer[1] = 127;
				std::memcpy(&send_buffer[2], &data_len, 8);
			}
		}
		else
		{
			// If the number is <= 125, it's the size
			send_buffer[1] = message.buffer.size();
		}

		if (use_nonce)
		{
			// Set nonce flag
			send_buffer[1] |= 0x80;

			uint8_t *nonce = &send_buffer[2];
			for (size_t i = 0; i < 4; i++)
			{
				// Randomize nonce - no fancy stuff here for simplicity
				nonce[i] = (rand() & 0xFF);
			}
			for (size_t i = send_header_len, j = 0; i < send_buffer.size(); i++, j++)
			{
				// XOR with the nonce
				send_buffer[i] ^= nonce[j & 3];
			}
		}

		if (send(sock, reinterpret_cast<char *>(&send_buffer[0]), send_buffer.size(), 0) == SOCKET_ERROR)
		{
#ifdef _WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
			if (errno == EWOULDBLOCK)
#endif
			{
				// It's a REAL error
				state = ClientState::CLOSING;
				return;
			}
		}
	}

	inline void Client::update_handshake()
	{
		int result = recv(sock, reinterpret_cast<char *>(&http_buffer[write_head]), http_buffer.size() - write_head, 0);
		if (result == SOCKET_ERROR)
		{
#ifdef _WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
			if (errno == EWOULDBLOCK)
#endif
			{
				// It's a REAL error
				state = ClientState::CLOSING;
				return;
			}
		}
		else if (result > 0)
		{
			write_head += result;
			if (write_head >= http_buffer.size())
			{
				// Who the hell sends over 64 KiB through a HTTP request?
				state = ClientState::CLOSING;
				return;
			}
		}
		if (http_buffer.size() < 4)
		{
			if (timeout > 1) // Hope you set one
			{
				if (std::chrono::steady_clock::now() - last_pong > std::chrono::seconds(timeout))
				{
					// Don't wait forever
					state = ClientState::CLOSING;
				}
			}
			return;
		}

		// This should work for both requests and responses due to its non-strict implementation
		for (size_t i = 0; i <= http_buffer.size() - 4; i++)
		{
			if (http_buffer[i] == '\r' && http_buffer[i + 1] == '\n' && http_buffer[i + 2] == '\r' && http_buffer[i + 3] == '\n')
			{
				std::string full_request = std::string(reinterpret_cast<char *>(&http_buffer[0]), i);
				if (full_request.empty())
				{
					state = ClientState::CLOSING;
					return;
				}

				HTTPMessage request;
				std::stringstream request_stream(full_request);
				std::string line;
				// Process the lines into a nice kv map
				while (std::getline(request_stream, line, '\n'))
				{
					size_t key_ind = line.find(':');
					if (key_ind != std::string::npos)
					{
						std::string key = line.substr(0, key_ind);
						std::transform(key.begin(), key.end(), key.begin(), [](char c)
									   { return static_cast<char>(std::tolower(c)); });
						std::string val = line.substr(key_ind + 1);
						size_t nospace = val.find_first_not_of(' ');
						if (nospace != std::string::npos)
							val = val.substr(nospace);

						// Weak \r\n removal
						if (!val.empty() && val.back() == '\n')
							val = val.substr(0, val.size() - 1);
						if (!val.empty() && val.back() == '\r')
							val = val.substr(0, val.size() - 1);

						request.headers[key] = val;
					}
				}

				// At least these headers should be present
				if (request.headers.find("upgrade") != request.headers.end() &&
					request.headers["upgrade"] == "websocket" &&
					request.headers.find("connection") != request.headers.end() &&
					request.headers["connection"] == "Upgrade")
				{
					if (request.headers.find("sec-websocket-key") != request.headers.end())
					{
						// A server requires a key from the client to accept the connection
						// After receiving it, the server sends a base64(SHA1(key + magic)) as a response
						constexpr std::string_view magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
						auto hash = SHA1().digest(request.headers["sec-websocket-key"] + std::string(magic));
						std::string accept_str = base64_encode(hash);

						std::string response =
							"HTTP/1.1 101 Switching Protocols\r\n"
							"Upgrade: websocket\r\n"
							"Connection: Upgrade\r\n"
							"Sec-WebSocket-Accept: " +
							accept_str +
							"\r\n"
							"Sec-WebSocket-Protocol: chat\r\n\r\n";

						send(sock, response.c_str(), response.size(), 0);

						// Connection has been established
						state = ClientState::ACTIVE;
					}
					else if (request.headers.find("sec-websocket-accept") != request.headers.end())
					{
						// The accept key from the server must match the expected key. A specially
						// crafted request with an empty accept key could mess up the server, so
						// we handle that first.
						if (request.headers["sec-websocket-accept"].empty())
						{
							state = ClientState::CLOSING;

							// Nothing to do, bail out
							return;
						}
						else if (request.headers["sec-websocket-accept"] != expect_accept_str)
						{
							state = ClientState::CLOSING;

							// Nothing to do, bail out
							return;
						}

						// Seems legit
						state = ClientState::ACTIVE;
					}
					else
					{
						// This is what happens to people that don't obey
						state = ClientState::CLOSING;
					}

					if (state == ClientState::ACTIVE && write_head > i + 4)
					{
						// Prevent data loss caused by reading too much
						wsbuffer.resize(write_head - i - 4);
						std::memcpy(&wsbuffer[0], &http_buffer[i + 4], wsbuffer.size());
					}
				}
				if (state == ClientState::ACTIVE)
					return;
				// We're highly likely the server if we got here. Send 404.
				std::string response = "HTTP/1.1 404 Not Found\r\nServer: MARCWS/1.0.0\r\nContent-Length: 13\r\n404 Not Found\r\n\r\n";
				send(sock, response.c_str(), response.size(), 0);
				return;
			}
		}
	}

	inline void Client::ping()
	{
		// 0xBADBEEF5 "Bad Beefs" is not meaningful regarding the implementation
		// This is arbitrary data with a maximum length of 125 bytes
		std::vector<uint8_t> data{0xBA, 0xDB, 0xEE, 0xF5, 0x00};

		// Those who remove the min operation and write past the limit shall perish
		size_t count = std::min<size_t>(data.size(), 125);

		std::vector<uint8_t> send_buffer;
		size_t send_header_len = 2 + (use_nonce ? 4 : 0);
		send_buffer.resize(send_header_len + count);
		std::memcpy(&send_buffer[send_header_len], &data[0], count);

		// 0x09 = Ping, 0x80 = Final frame
		send_buffer[0] = 0x09 | 0x80;

		send_buffer[1] = count;

		if (use_nonce)
		{
			// Set the nonce flag
			send_buffer[1] |= 0x80;

			uint8_t *nonce = &send_buffer[2];
			for (size_t i = 0; i < 4; i++)
			{
				// Randomize nonce - no fancy stuff here for simplicity
				nonce[i] = rand() & 0xFF;
			}

			for (size_t i = send_header_len, j = 0; i < send_buffer.size(); i++, j++)
			{
				// XOR with the nonce
				send_buffer[i] ^= nonce[j & 3];
			}
		}

		send(sock, reinterpret_cast<char *>(send_buffer.data()), send_buffer.size(), 0);
	}

	inline void Client::pong(uint8_t *data, size_t count)
	{
		// Can't trust nobody these days
		count = std::min<size_t>(count, 125);

		// Respond to the ping with the same content
		std::vector<uint8_t> send_buffer;

		// 2 bytes for the header and optionally the nonce
		size_t send_header_len = 2 + (use_nonce ? 4 : 0);
		send_buffer.resize(send_header_len + count);

		// Copy the data in
		std::memcpy(&send_buffer[send_header_len], data, count);

		// 0x0A = Pong, 0x80 = Final frame
		send_buffer[0] = 0x0A | 0x80;

		send_buffer[1] = count;
		if (use_nonce)
		{
			// Set the nonce flag
			send_buffer[1] |= 0x80;

			uint8_t *nonce = &send_buffer[2];
			for (size_t i = 0; i < 4; i++)
			{
				// Randomize nonce - no fancy stuff here for simplicity
				nonce[i] = (rand() & 0xFF);
			}
			for (size_t i = send_header_len, j = 0; i < send_buffer.size(); i++, j++)
			{
				// XOR with the nonce
				send_buffer[i] ^= nonce[j & 3];
			}
		}
		send(sock, reinterpret_cast<char *>(send_buffer.data()), send_buffer.size(), 0);
	}

	inline void Client::update_websocket()
	{
		int result = 0;
		do
		{
			uint8_t recv_buffer[256];
			result = recv(sock, reinterpret_cast<char *>(recv_buffer), sizeof(recv_buffer), 0);
			if (result < 0)
			{
				if (result == SOCKET_ERROR)
				{
#ifdef _WIN32
					if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
					if (errno == EWOULDBLOCK)
#endif
						break;
				}

				// Terminate the connection to prevent further errors
				state = ClientState::CLOSING;
				return;
			}
			else if (result > 0)
			{
				size_t old_size = wsbuffer.size();
				wsbuffer.resize(wsbuffer.size() + result);
				std::memcpy(&wsbuffer[old_size], recv_buffer, result);
			}
		} while (result > 0);

		if (timeout > 1)
		{
			if (std::chrono::steady_clock::now() - last_ping > std::chrono::seconds((timeout + 1) / 2))
			{
				ping();
				last_ping = std::chrono::steady_clock::now();
			}
			if (std::chrono::steady_clock::now() - last_pong > std::chrono::seconds(timeout))
			{
				// No pong response in a while
				state = ClientState::CLOSING;
				return;
			}
		}

		// Handle messages
		while (true)
		{
			size_t header_len = 2;
			if (wsbuffer.size() < header_len)
				return;

			uint8_t byte0 = wsbuffer[0];

			bool final_frame = (byte0 >> 7);
			if ((byte0 & 0x70) != 0)
			{
				// Must terminate if not zero as per specification
				state = ClientState::CLOSING;
				return;
			}
			uint8_t opcode = (byte0 & 15);
			uint8_t byte1 = wsbuffer[1];
			bool masked = (byte1 & 0x80) != 0;
			if (masked == use_nonce)
			{
				// Client must mask the message
				state = ClientState::CLOSING;
				return;
			}
			uint8_t msglen_pre = (byte1 & 127);
			size_t msg_len;
			switch (msglen_pre)
			{
			case 126:
			{
				header_len += 2;
				if (wsbuffer.size() < header_len)
					return; // Not enough data
				uint16_t len;
				std::memcpy(&len, &wsbuffer[2], 2);
				msg_len = ntohs(len);
				break;
			}
			case 127:
			{
				header_len += 8;
				if (wsbuffer.size() < header_len)
					return; // Not enough data
				uint64_t len;
				std::memcpy(&len, &wsbuffer[2], 8);
				msg_len = ntohll(len) & 0x7FFFFFFFFFFFFFFF;
				break;
			}
			default:
				msg_len = msglen_pre;
				break;
			}
			uint8_t mask_key[4]{0, 0, 0, 0};
			if (masked)
			{
				header_len += 4;

				if (wsbuffer.size() < header_len)
					return; // Not enough data

				std::memcpy(mask_key, &wsbuffer[header_len - 4], 4);
			}
			if (wsbuffer.size() < header_len + msg_len)
				return; // Not enough data
			switch (opcode)
			{
			case 0:
			{
				// Continuation frame
				msg.append(&wsbuffer[header_len], msg_len, masked ? mask_key : nullptr);
				break;
			}
			case 1:
			{
				// Text
				msg.clear();
				msg.append(&wsbuffer[header_len], msg_len, masked ? mask_key : nullptr);
				msg.type = MessageType::UTF8;
				break;
			}
			case 2:
			{
				// Binary data - same as text, separated for clarity
				msg.clear();
				msg.append(&wsbuffer[header_len], msg_len, masked ? mask_key : nullptr);
				msg.type = MessageType::BINARY;
				break;
			}
			case 8:
			{
				// Close
				std::string status_message = "Connection closed.";
				if (msg_len > 0)
				{
					uint16_t status_len;
					std::memcpy(&status_len, &wsbuffer[header_len], 2);
					status_len = std::min<uint16_t>(123, ntohs(status_len));
					if (wsbuffer.size() >= header_len + 2 + status_len)
						status_message = std::string(reinterpret_cast<char *>(&wsbuffer[header_len + 2]), status_len);
				}

				if (close_handler)
					close_handler(*this, status_message);

				break;
			}
			case 9:
			{
				// Ping
				if (msg_len > 125)
				{
					// Message too long
					state = ClientState::CLOSING;
					return;
				}
				pong(&wsbuffer[header_len], msg_len);
				break;
			}
			case 10:
				// Pong
				last_pong = std::chrono::steady_clock::now();
				break;
			default:
				break;
			}
			if (final_frame)
			{
				if (opcode <= 2 && msg_handler)
					msg_handler(*this, msg);
			}
			wsbuffer.erase(wsbuffer.begin(), wsbuffer.begin() + (header_len + msg_len));
		}
	}

	inline void Client::start(const std::string &server_ip, uint16_t server_port)
	{
		if (IS_VALIDSOCK(sock))
			return;
		use_nonce = true;

		// Creating socket file descriptor
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (!IS_VALIDSOCK(sock))
		{
			throw std::runtime_error("Could not create socket.");
		}

		sockaddr_in server_addr;
		server_addr.sin_family = AF_INET;
		server_addr.sin_port = htons(server_port);
		lookup_host(server_ip, &server_addr.sin_addr);

		// Connect to server
		if (connect(sock, reinterpret_cast<sockaddr *>(&server_addr), sizeof(sockaddr_in)) < 0)
			throw std::runtime_error("Failed to connect to server");

		// Despite its name, isn't actually secure
		// Could technically seed the PRNG here to avoid caching issues
		std::array<uint8_t, 16> sec_key;
		for (size_t i = 0; i < sec_key.size(); i++)
			sec_key[i] = rand() & 0xFF;

		std::string sec_key_str = base64_encode(sec_key);

		// HTTP Request that should be fine for most WS servers
		std::string req =
			"GET / HTTP/1.1\r\n"
			"Host: " +
			server_ip +
			"\r\n"
			"User-Agent: MARCWS/1.0.0\r\n"
			"Accept: */*\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: " +
			sec_key_str + "\r\n\r\n";

		send(sock, req.c_str(), req.size(), 0);

		constexpr std::string_view magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		auto hash = SHA1().digest(sec_key_str + std::string(magic));
		expect_accept_str = base64_encode(hash);

		// Enable non-blocking socket
		u_long mode = 1;
		ioctlsocket(sock, FIONBIO, &mode);

		state = ClientState::HANDSHAKE;
	}

	inline void Client::update()
	{
		switch (state)
		{
		case ClientState::HANDSHAKE:
			update_handshake();
			break;
		case ClientState::ACTIVE:
			update_websocket();
			break;
		case ClientState::CLOSING:
			stop();
			break;
		default:
			break;
		}
	}

	inline void Client::stop()
	{
		if (IS_VALIDSOCK(sock))
			closesocket(sock);
		sock = INVALID_SOCKET;
		state = ClientState::CLOSED;
	}

	inline void Client::set_timeout(uint32_t seconds)
	{
		timeout = seconds;
	}

	inline void Server::start(uint16_t port)
	{
		if (IS_VALIDSOCK(server_fd))
			return;

		// Creating socket file descriptor
		server_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (!IS_VALIDSOCK(server_fd))
		{
			throw std::runtime_error("Could not create server socket.");
		}

		// Enable non-blocking socket
		u_long mode = 1;
		ioctlsocket(server_fd, FIONBIO, &mode);

		int opt = 1;

		// Bind to address and port
		if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt)))
		{
			closesocket(server_fd);
			server_fd = INVALID_SOCKET;
			throw std::runtime_error("Failed to set socket options.");
		}
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons(port);
		if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		{
			closesocket(server_fd);
			server_fd = INVALID_SOCKET;
			throw std::runtime_error("Failed to bind socket.");
		}
		// Start listening for incoming connections
		if (listen(server_fd, 10) < 0)
		{
			closesocket(server_fd);
			server_fd = INVALID_SOCKET;
			throw std::runtime_error("Failed to listen.");
		}
	}

	inline void Server::update()
	{
		// Update only the pre-existing clients - this way the user
		// can properly initialize clients before the first update
		for (auto &client : clients)
		{
			client.update();
		}

		socklen_t addrlen = sizeof(sockaddr_in);
		SOCKET new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);

		// Accept incoming connection
		if (IS_VALIDSOCK(new_socket))
			clients.emplace_back(new_socket).state = ClientState::HANDSHAKE;

		// Remove closed clients
		const auto filter_closed = [](Client &val)
		{
			return val.state == ClientState::CLOSING;
		};
		clients.erase(std::remove_if(clients.begin(), clients.end(), filter_closed), clients.end());
	}

	inline void Server::set_timeout(uint32_t seconds)
	{
		timeout = seconds;
		for (Client &client : clients)
		{
			client.set_timeout(seconds);
		}
	}

	inline void Server::stop()
	{
		clients.clear();

		if (IS_VALIDSOCK(server_fd))
			closesocket(server_fd);
		server_fd = INVALID_SOCKET;
	}

	inline Server::Server()
	{
	}

	inline Server::~Server()
	{
		stop();
	}

	inline void _deinit()
	{
#ifdef _WIN32
		WSACleanup();
#endif
	}

	inline void init()
	{
#ifdef _WIN32
		WSADATA wsadata;
		if (WSAStartup(0x0202, &wsadata) != 0)
			throw std::runtime_error("Failed to start winsock.");
		atexit(_deinit);
#endif
	}

} // namespace WebSocket
