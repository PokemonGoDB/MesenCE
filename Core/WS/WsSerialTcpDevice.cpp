#include "pch.h"
#include "WS/WsSerialTcpDevice.h"
#include "Shared/MessageManager.h"
#include "Utilities/Socket.h"

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
	std::lock_guard<std::mutex> lock(_lock);
	_toServer.push_back(value);
}

bool WsSerialTcpDevice::Receive(uint8_t& value, uint64_t masterClock)
{
	if(_pending == 0) {
		return false;
	}
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
