#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include "WS/WsSerialDevice.h"

//Connects the console's serial line to a TCP server (a WonderGate adapter program, a link-cable
//relay, PC transfer tools). Bytes go both ways unchanged; the network runs on its own thread so
//the emulation never waits on it, except to keep the console in real time while the other end owes
//it an answer (see Receive). Reconnects if the connection drops.
class WsSerialTcpDevice final : public IWsSerialDevice
{
private:
	string _host;
	uint16_t _port = 0;

	std::thread _thread;
	std::atomic<bool> _stop = false;
	std::mutex _lock;
	std::deque<uint8_t> _toConsole;
	std::deque<uint8_t> _toServer;
	std::condition_variable _arrived;
	std::atomic<uint32_t> _pending = 0;

	//The console sent bytes the other end hasn't answered yet (emulation thread only)
	bool _awaitingReply = false;
	uint64_t _sendClock = 0;
	std::chrono::steady_clock::time_point _sendTime;

	void Run();

public:
	WsSerialTcpDevice(const string& host, uint16_t port);
	~WsSerialTcpDevice();

	void OnSend(uint8_t value, uint64_t masterClock) override;
	bool Receive(uint8_t& value, uint64_t masterClock) override;

	//"host:port" from the MESEN_WS_SERIAL_TCP environment variable, or null when it isn't set
	static unique_ptr<IWsSerialDevice> FromEnvironment();
};
