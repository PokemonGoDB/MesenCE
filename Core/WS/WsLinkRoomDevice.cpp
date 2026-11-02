#include "pch.h"
#include "WS/WsLinkRoomDevice.h"
#include "Shared/MessageManager.h"
#include "Utilities/Socket.h"

//The WonderSwan's master clock
static constexpr uint64_t ClocksPerSecond = 3072000;
static constexpr uint64_t ClocksPerMs = ClocksPerSecond / 1000;
//A batch of the console's bytes goes to the room once its first byte is this old, or it's this big (as the
//virtual module and the site's Batcher)
static constexpr auto BatchWait = std::chrono::milliseconds(4);
static constexpr size_t BatchMax = 64 * 6;
//How the jitter buffer follows a sender whose clock runs slow: every 2 s, shrink by up to 1 ms
static constexpr uint64_t DriftWindowClocks = 2 * ClocksPerSecond;
static constexpr int64_t DriftStepClocks = (int64_t)ClocksPerMs;

WsLinkRoomDevice::WsLinkRoomDevice(uint16_t relayPort)
{
	_relayPort = relayPort;
	_thread = std::thread([this]() { Run(); });
}

WsLinkRoomDevice::~WsLinkRoomDevice()
{
	_stop = true;
	if(_thread.joinable()) {
		_thread.join();
	}
}

void WsLinkRoomDevice::Pick(uint64_t masterClock)
{
	int command = _command.exchange(0);
	if(command == 1) {
		_started = true;
		_startClock = masterClock;
		_due.clear();
		_haveOffset = _haveLast = _havePrev = false;
		_senderUs = 0;
		_spare = INT64_MAX;
		_late = 0;
		MessageManager::Log("[WS] Link play: linked");
	} else if(command == 2) {
		if(_started) {
			MessageManager::Log("[WS] Link play: unlinked (" + std::to_string(_late) + " bytes arrived late)");
		}
		_started = false;
		_due.clear();
		std::lock_guard<std::mutex> lock(_lock);
		_fromRoom.clear();
		_toRoom.clear();
	}
}

void WsLinkRoomDevice::OnSend(uint8_t value, uint64_t masterClock)
{
	Pick(masterClock);
	if(!_started) {
		return; //silent until the start: the byte goes nowhere, as with no cable
	}
	uint32_t time = (uint32_t)((masterClock - _startClock) * 1000000 / ClocksPerSecond);
	std::lock_guard<std::mutex> lock(_lock);
	if(_toRoom.empty()) {
		_toRoomSince = std::chrono::steady_clock::now();
	}
	_toRoom.push_back(time & 0xFF);
	_toRoom.push_back((time >> 8) & 0xFF);
	_toRoom.push_back((time >> 16) & 0xFF);
	_toRoom.push_back((time >> 24) & 0xFF);
	_toRoom.push_back(value);
	_toRoom.push_back(_fast ? 1 : 0);
}

uint64_t WsLinkRoomDevice::DueClock(const Record& r, uint64_t now)
{
	//The sender's clock, followed across the 32-bit wrap (71 minutes)
	_senderUs = _haveLast ? _senderUs + (uint32_t)(r.Time - _lastTime) : r.Time;
	_haveLast = true;
	_lastTime = r.Time;
	int64_t sender = (int64_t)(_senderUs * ClocksPerSecond / 1000000);
	int64_t jitter = (int64_t)_jitterMs * (int64_t)ClocksPerMs;
	if(!_haveOffset) {
		_offset = (int64_t)now - sender + jitter;
		_haveOffset = true;
		_windowStart = now;
	}
	//Never before the previous byte plus a byte time; a late one plays now and pushes the rest back
	int64_t due = sender + _offset;
	if(_havePrev) {
		due = std::max(due, (int64_t)_prevDue + (r.Fast ? 800 : 3200));
	}
	if(due < (int64_t)now) {
		_late++;
		_offset += (int64_t)now - due;
		due = (int64_t)now;
	}
	//Two clocks drift: when records keep arriving with more than the allowance to spare, shrink back a little
	_spare = std::min(_spare, due - (int64_t)now - jitter);
	if(now - _windowStart >= DriftWindowClocks) {
		if(_spare > 0 && _spare != INT64_MAX) {
			_offset -= std::min(_spare, DriftStepClocks);
		}
		_spare = INT64_MAX;
		_windowStart = now;
	}
	_havePrev = true;
	_prevDue = (uint64_t)due;
	return _prevDue;
}

bool WsLinkRoomDevice::Receive(uint8_t& value, uint64_t masterClock)
{
	Pick(masterClock);
	if(!_started) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_lock);
		while(!_fromRoom.empty()) {
			Record r = _fromRoom.front();
			_fromRoom.pop_front();
			_due.push_back({ DueClock(r, masterClock), r.Value });
		}
	}
	if(_due.empty() || _due.front().Clock > masterClock) {
		return false;
	}
	value = _due.front().Value;
	_due.pop_front();
	return true;
}

void WsLinkRoomDevice::Run()
{
	unique_ptr<Socket> socket;
	vector<uint8_t> in;

	while(!_stop) {
		if(!socket) {
			socket.reset(new Socket());
			if(_relayPort == 0 || !socket->Connect("127.0.0.1", _relayPort) || socket->ConnectionError()) {
				socket.reset();
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			char hello[3] = { 'L', 'K', 1 };
			socket->Send(hello, sizeof(hello), 0);
			in.clear();
			MessageManager::Log("[WS] Link play: cable plugged in");
		}

		vector<uint8_t> out;
		{
			std::lock_guard<std::mutex> lock(_lock);
			if(!_toRoom.empty() && (_toRoom.size() >= BatchMax || std::chrono::steady_clock::now() - _toRoomSince >= BatchWait)) {
				out.swap(_toRoom);
			}
		}
		if(!out.empty()) {
			vector<uint8_t> frame = { 'D', (uint8_t)(out.size() & 0xFF), (uint8_t)(out.size() >> 8) };
			frame.insert(frame.end(), out.begin(), out.end());
			socket->Send((char*)frame.data(), (int)frame.size(), 0);
		}

		char buffer[2048];
		int received = socket->Recv(buffer, sizeof(buffer), 0);
		if(received > 0) {
			in.insert(in.end(), buffer, buffer + received);
			//Whole frames only; a partial one waits for the rest
			size_t at = 0;
			while(in.size() - at >= 3) {
				uint8_t type = in[at];
				size_t length = in[at + 1] | (in[at + 2] << 8);
				if(in.size() - at - 3 < length) {
					break;
				}
				const uint8_t* p = in.data() + at + 3;
				if(type == 'S' && length >= 2) {
					_keepNewer = (p[0] & 1) != 0;
					_fast = (p[0] & 2) != 0;
					_jitterMs = p[1];
					_command = 1;
				} else if(type == 'X') {
					_command = 2;
				} else if(type == 'D') {
					std::lock_guard<std::mutex> lock(_lock);
					for(size_t i = 0; i + 6 <= length; i += 6) {
						uint32_t time = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
						_fromRoom.push_back({ time, p[i + 4], (p[i + 5] & 1) != 0 });
					}
				}
				at += 3 + length;
			}
			in.erase(in.begin(), in.begin() + at);
		} else if(received == 0 || socket->ConnectionError()) {
			//The relay went (the player left the room, or the UI closed): back to silent
			MessageManager::Log("[WS] Link play: cable unplugged");
			_command = 2;
			socket.reset();
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
