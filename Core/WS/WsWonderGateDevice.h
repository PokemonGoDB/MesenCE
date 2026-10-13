#pragma once
#include "pch.h"
#include <deque>
#include <fstream>
#include "WS/WsSerialDevice.h"
extern "C"
{
#include "Utilities/WonderGate/wg.h"
}

class Socket;

//The MobileWonderGate adapter (Bandai, 2001), emulated in the EXT port.
//The adapter protocol is wgcore (Utilities/WonderGate). Its TCP connections go to local relay
//relay port opened by the UI, which carries them to a WonderGate server; every hostname resolves
//there, so the emulator never connects anywhere else on its own. Each connection starts with a
//4-byte preamble naming the port the game dialled: 'W' 'G' port-high port-low.
class WsWonderGateDevice final : public IWsSerialDevice
{
private:
	struct Connection
	{
		unique_ptr<Socket> Sock;
		bool AwaitingReply = false;
	};

	wg _gate = {};
	wg_ops _ops = {};
	wg_config _config = {};

	std::deque<uint8_t> _toConsole;
	Connection _connections[WG_MAX_SOCKS * 2];
	uint16_t _relayPort = 0;

	uint64_t _lastSendClock = 0;
	std::ofstream _trace;

	static void Tx(void* ctx, const uint8_t* buf, size_t len);
	static int Resolve(void* ctx, const char* name, uint8_t ip[4]);
	static int SockOpen(void* ctx);
	static int SockConnect(void* ctx, int h, const uint8_t ip[4], uint16_t port);
	static int SockSend(void* ctx, int h, const uint8_t* buf, size_t len);
	static int SockRecv(void* ctx, int h, uint8_t* buf, size_t len);
	static void SockShutdown(void* ctx, int h, int how);
	static void SockClose(void* ctx, int h);
	static void Log(void* ctx, const char* line);

	Connection* Get(int h);

public:
	WsWonderGateDevice(uint16_t relayPort);
	~WsWonderGateDevice();

	void OnSend(uint8_t value, uint64_t masterClock) override;
	bool Receive(uint8_t& value, uint64_t masterClock) override;
};
