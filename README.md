# Mini-WS
A dependencyless minimal POSIX/Windows implementation of the WebSocket Protocol in C++17

## Why?
My honest answer is: I was too lazy to look for an existing implementation that meets the following criteria:
- You plop the files in, include a header, and start using it.
- It doesn't have a bloated dependency chain. 
- It only contains what the user actually needs.
- It doesn't require a massive toolchain to build.
- It works on at least both Linux and Windows without extra effort.

## Example Usage

Clueless? No worries, here's some boilerplate code to get you started:

```cpp

#include <iostream>
#include <unistd.h>

// #define WEBSOCKET_HEADER_ONLY // Use this for a single-include
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
    // Better call this before using the library
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

        // THE JUICY PART

        // Send a message to server every 20 "ticks"
        if (++counter >= 20)
        {
            counter %= 20;

            // Build a message
            std::string msg_str = "abc " + std::to_string(rand() & 0xFF);
            std::vector<uint8_t> msg;
            msg.resize(msg_str.size());
            std::memcpy(msg.data(), msg_str.data(), msg_str.size());

            c.send_message(WebSocket::Message(WebSocket::MessageType::UTF8, msg));
        }

        // Register client message handler
        for (WebSocket::Client &client : server.clients)
        {
            if (!client.msg_handler)
                client.msg_handler = on_server_message;
        }

        // END OF THE JUICY PART

        // Every "tick" here lasts for roughly 50ms. This is by no means required.
        usleep(50000);
    }
    return 0;
}
```

## Notice
This is not a perfect nor a full implementation of the WebSocket Protocol. Most notably it has no capability to fragment any messages when sending.
It works well enough for my use cases, and that's enough for me. It is up to you (the user of this library) to apply any fixes or changes to your copy of the software, conforming to LICENSE.txt
