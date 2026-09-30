// client.cpp - Interactive libssh client (C++20)
// Usage: set environment variables or edit the defaults below.
//
// Quick start (PowerShell, Windows):
// 1) Ensure the server is running and has generated a host key (see server README comments).
// 2) Set client environment variables or edit defaults in this file:
//    $env:SSH_SERVER_HOST='127.0.0.1'; $env:SSH_SERVER_PORT='2222';
//    $env:SSH_USER='tester'; $env:SSH_PASS='tester'
// 3) Build the client project in Visual Studio with libssh includes and libs.
// 4) Run client.exe from the same PowerShell where env vars are set.
//
// Visual Studio settings:
// - C/C++ -> Additional Include Directories: path to libssh includes
// - Linker -> Additional Library Directories: path to libssh libs
// - Linker -> Additional Dependencies: libssh.lib libcrypto.lib libssl.lib ws2_32.lib
// - Target Platform: x64 (match libssh build)
//
/*
 Port-forwarding and public-key auth notes (client):

 - Local port forwarding (client-side "-L") requires the client to open a
   direct-tcpip channel to the server for each forwarded connection and then
   bridge local socket IO to that channel. libssh provides APIs to open
   "direct-tcpip" channels; implementing a robust forwarder requires socket
   code and concurrency handling.

 - Public-key auth: instead of ssh_userauth_password, call
   ssh_userauth_publickey(session, NULL, privatekey) or use ssh_key APIs to
   load a private key and perform authentication. For known_hosts verification
   the example already writes the host key on first connect.

 This example focuses on interactive shell auth via password for learning.
 For port-forwarding experiments, consider using OpenSSH client/server or
 extend this code with explicit socket bridging.
*/

#include <libssh/libssh.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

static int verify_knownhost(ssh_session session)
{
	int state = ssh_is_server_known(session);
	switch (state)
	{
	case SSH_SERVER_KNOWN_OK:
		return 0;
	case SSH_SERVER_NOT_KNOWN:
		if (ssh_write_knownhost(session) < 0)
		{
			std::cerr << "Failed to add host to known_hosts\n";
			return -1;
		}
		return 0;
	default:
		std::cerr << "Host key verification failed (state=" << state << ")\n";
		return -1;
	}
}

// Convert the standalone client into a callable function so the file can be
// compiled into the server project without introducing a duplicate 'main'.
// To build the client as a standalone executable, define BUILD_CLIENT in the
// client project settings (or add -DBUILD_CLIENT) and a main() wrapper will be
// provided below.
int client_main()
{
	// Read from environment or use defaults for quick testing
	auto getenv_str = [](const char* name) -> std::string {
#ifdef _WIN32
		char* buffer = nullptr;
		size_t len = 0;
		if (_dupenv_s(&buffer, &len, name) == 0 && buffer) {
			std::string result(buffer);
			free(buffer);
			return result;
		}
		return {};
#else
		const char* v = std::getenv(name);
		return v ? std::string(v) : std::string{};
#endif
	};

	/*std::string host = getenv_str("SSH_SERVER_HOST");
	if (host.empty()) host = "127.0.0.1";
	std::string port_s = getenv_str("SSH_SERVER_PORT");
	int port = port_s.empty() ? 2222 : std::atoi(port_s.c_str());
	std::string user = getenv_str("SSH_USER");
	if (user.empty()) user = "tester";
	std::string password = getenv_str("SSH_PASS");
	if (password.empty()) password = "tester";*/

	// Fixed connection settings
	std::string host = "127.0.0.1";
	int port = 2222;
	std::string user = "tester";
	std::string password = "tester";

	ssh_session session = ssh_new();
	if (!session) {
		std::cerr << "Failed to allocate SSH session\n";
		return EXIT_FAILURE;
	}

	ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
	ssh_options_set(session, SSH_OPTIONS_PORT, &port);
	ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());

	if (ssh_connect(session) != SSH_OK) {
		std::cerr << "Error connecting: " << ssh_get_error(session) << "\n";
		ssh_free(session);
		return EXIT_FAILURE;
	}

	if (verify_knownhost(session) < 0) {
		ssh_disconnect(session);
		ssh_free(session);
		return EXIT_FAILURE;
	}

	if (ssh_userauth_password(session, NULL, password.c_str()) != SSH_AUTH_SUCCESS) {
		std::cerr << "Authentication failed: " << ssh_get_error(session) << "\n";
		ssh_disconnect(session);
		ssh_free(session);
		return EXIT_FAILURE;
	}

	std::cout << "Authenticated successfully. Opening channel...\n";

	ssh_channel channel = ssh_channel_new(session);
	if (!channel) {
		std::cerr << "Failed to create channel\n";
		ssh_disconnect(session);
		ssh_free(session);
		return EXIT_FAILURE;
	}

	if (ssh_channel_open_session(channel) != SSH_OK) {
		std::cerr << "ssh_channel_open_session failed: " << ssh_get_error(session) << "\n";
		ssh_channel_free(channel);
		ssh_disconnect(session);
		ssh_free(session);
		return EXIT_FAILURE;
	}

	// Request PTY and shell
	ssh_channel_request_pty(channel);
	if (ssh_channel_request_shell(channel) != SSH_OK) {
		std::cerr << "ssh_channel_request_shell failed: " << ssh_get_error(session) << "\n";
		ssh_channel_close(channel);
		ssh_channel_free(channel);
		ssh_disconnect(session);
		ssh_free(session);
		return EXIT_FAILURE;
	}

	std::atomic<bool> running{true};

	// Thread: channel -> stdout
	std::thread reader([&]() {
		const int BUF_SIZE = 4096;
		std::string buffer;
		buffer.resize(BUF_SIZE);
		while (running) {
			int n = ssh_channel_read(channel, buffer.data(), BUF_SIZE, 0);
			if (n > 0) {
				std::cout.write(buffer.data(), n);
				std::cout.flush();
			} else if (n == 0) {
				// channel closed
				break;
			} else {
				// error
				break;
			}
		}
		running = false;
	});

	// Thread: stdin -> channel
	std::thread writer([&]() {
#ifdef _WIN32
		// On Windows, use std::getline; this is simple but not a full terminal emulation.
		std::string line;
		while (running && std::getline(std::cin, line)) {
			line.push_back('\n');
			if (ssh_channel_write(channel, line.data(), line.size()) < 0) break;
		}
#else
		// POSIX: raw read
		while (running && !std::cin.eof()) {
			char c = std::cin.get();
			if (!std::cin) break;
			if (ssh_channel_write(channel, &c, 1) < 0) break;
		}
#endif
		// Signal EOF to server side
		ssh_channel_send_eof(channel);
		running = false;
	});

	// Wait for threads to finish
	if (writer.joinable()) writer.join();
	if (reader.joinable()) reader.join();

	ssh_channel_close(channel);
	ssh_channel_free(channel);

	ssh_disconnect(session);
	ssh_free(session);

	return EXIT_SUCCESS;
}

// Provide a main() entry point that calls client_main when building the
// standalone client. If you need to compile this file into another
// project that already provides main(), define BUILD_NO_CLIENT_MAIN in the
// consuming project's preprocessor definitions to suppress this wrapper.
#if defined(BUILD_CLIENT) || !defined(BUILD_NO_CLIENT_MAIN)
int main() {
	return client_main();
}
#endif
