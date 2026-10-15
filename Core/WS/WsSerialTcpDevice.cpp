#include "pch.h"
#include "WS/WsSerialTcpDevice.h"
#include "Shared/MessageManager.h"
#include "Utilities/Socket.h"

//The WonderSwan's master clock
static constexpr uint64_t ClocksPerSecond = 3072000;

//How long the console is kept in real time waiting for an answer. Longer than any wait of a
//WonderGate adapter program (wgcore-host waits up to 2 s per receive, a few times for HTTP); a peer
//that never answers (a link cable with nobody on the other end) only slows emulation this long.
static constexpr uint64_t MaxReplyWaitClocks = ClocksPerSecond * 10;

WsSerialTcpDevice::WsSerialTcpDevice(const string& host, uint16_t port)
{
	_host = host;
	_port = port;
	_thread = std::thread([this]() { Run(); });
}

WsSerialTcpDevice::~WsSerialTcpDevice()
{
	_stop = true;
	if(_thread.joinable()) {
		_thread.join();
	}
}

void WsSerialTcpDevice::OnSend(uint8_t value, uint64_t masterClock)
{
	if(!_awaitingReply) {
		_awaitingReply = true;
		_sendClock = masterClock;
		_sendTime = std::chrono::steady_clock::now();
	}
	std::lock_guard<std::mutex> lock(_lock);
	_toServer.push_back(value);
}

bool WsSerialTcpDevice::Receive(uint8_t& value, uint64_t masterClock)
{
	if(_pending == 0) {
		//The other end (a WonderGate adapter program waiting on its server, say) answers in real
		//time. An emulator running faster than real time (fast forward, the test runner) would make
		//it look slow to the game, which counts frames: hold the console to real time until the
		//answer comes. A real adapter's network wait costs a real console the same frames.
		if(_awaitingReply) {
			if(masterClock < _sendClock || masterClock - _sendClock > MaxReplyWaitClocks) {
				_awaitingReply = false; //no answer coming, or a save state was loaded
			} else {
				auto emulated = std::chrono::microseconds((masterClock - _sendClock) * 1000000 / ClocksPerSecond);
				auto ahead = emulated - (std::chrono::steady_clock::now() - _sendTime);
				if(ahead > std::chrono::milliseconds(2)) {
					std::unique_lock<std::mutex> lock(_lock);
					_arrived.wait_for(lock, ahead, [this]() { return _pending > 0 || _stop; });
				}
			}
		}
		if(_pending == 0) {
			return false;
		}
	}
	_awaitingReply = false;
	std::lock_guard<std::mutex> lock(_lock);
	value = _toConsole.front();
	_toConsole.pop_front();
	_pending--;
	return true;
}

void WsSerialTcpDevice::Run()
{
	unique_ptr<Socket> socket;
	string target = _host + ":" + std::to_string(_port);

	while(!_stop) {
		if(!socket) {
			socket.reset(new Socket());
			if(!socket->Connect(_host.c_str(), _port) || socket->ConnectionError()) {
				socket.reset();
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
			MessageManager::Log("[WS] Serial port connected to " + target);
		}

		vector<uint8_t> out;
		{
			std::lock_guard<std::mutex> lock(_lock);
			out.assign(_toServer.begin(), _toServer.end());
			_toServer.clear();
		}
		if(!out.empty()) {
			socket->Send((char*)out.data(), (int)out.size(), 0);
		}

		char buffer[1024];
		int received = socket->Recv(buffer, sizeof(buffer), 0);
		if(received > 0) {
			std::lock_guard<std::mutex> lock(_lock);
			_toConsole.insert(_toConsole.end(), buffer, buffer + received);
			_pending += received;
			_arrived.notify_all();
		} else if(received == 0 || socket->ConnectionError()) {
			MessageManager::Log("[WS] Serial port disconnected from " + target);
			socket.reset();
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

unique_ptr<IWsSerialDevice> WsSerialTcpDevice::FromEnvironment()
{
	string target = WsReadEnv("MESEN_WS_SERIAL_TCP");
	if(target.empty()) {
		return nullptr;
	}
	size_t colon = target.rfind(':');
	if(colon == string::npos) {
		MessageManager::Log("[WS] MESEN_WS_SERIAL_TCP should be host:port");
		return nullptr;
	}
	int port = std::atoi(target.substr(colon + 1).c_str());
	if(port <= 0 || port > 65535) {
		MessageManager::Log("[WS] MESEN_WS_SERIAL_TCP has an invalid port");
		return nullptr;
	}
	MessageManager::Log("[WS] Serial port: TCP " + target);
	return std::make_unique<WsSerialTcpDevice>(target.substr(0, colon), (uint16_t)port);
}
