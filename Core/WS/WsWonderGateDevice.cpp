#include "pch.h"
#include <chrono>
#include <thread>
#include <ctime>
#include "WS/WsWonderGateDevice.h"
#include "Utilities/Socket.h"

//A real adapter drops a half-received frame after a pause; about 100 ms of master clock
static constexpr uint64_t FrameGapClocks = 3072000 / 10;

//Replies start once the console has stopped sending (two byte times of quiet line at 9600 baud).
//Games don't read while they send: SD Gundam Gashapon Senki's wake-up burst of 00s takes about
//10 ms at the real baud rate, and an answer started during it is lost.
static constexpr uint64_t QuietLineClocks = 3200 * 2;

//How long a receive waits for a reply the game is expecting. A real adapter answered at once, but
//the network is slower than a local adapter; waiting here hides that from games with short
//timeouts (the emulation pauses briefly, as it would have on the phone line). Some games give up on
//the first empty reply, so this covers a whole first exchange on a slow connection: opening the
//tunnel (TLS) plus the server's work, often far from its database. Idle sockets that the game is
//only polling are answered immediately with "no data".
static constexpr int ReplyWaitMs = 6000;

WsWonderGateDevice::WsWonderGateDevice(uint16_t relayPort)
{
	_relayPort = relayPort;

	//Debug trace in the game server project's adapter.log format
	string tracePath = WsReadEnv("MESEN_WG_LOG");
	if(!tracePath.empty()) {
		_trace.open(tracePath, std::ios::app);
	}

	_ops.ctx = this;
	_ops.tx = &WsWonderGateDevice::Tx;
	_ops.resolve = &WsWonderGateDevice::Resolve;
	_ops.sock_open = &WsWonderGateDevice::SockOpen;
	_ops.sock_connect = &WsWonderGateDevice::SockConnect;
	_ops.sock_send = &WsWonderGateDevice::SockSend;
	_ops.sock_recv = &WsWonderGateDevice::SockRecv;
	_ops.sock_shutdown = &WsWonderGateDevice::SockShutdown;
	_ops.sock_close = &WsWonderGateDevice::SockClose;
	_ops.log = _trace.is_open() ? &WsWonderGateDevice::Log : nullptr;

	wg_config_defaults(&_config);
	wg_init(&_gate, &_ops, &_config);
	Log(this, "wgcore in MesenCE: adapter attached to the EXT port");
}

WsWonderGateDevice::~WsWonderGateDevice()
{
	for(Connection& c : _connections) {
		c.Sock.reset();
	}
}

void WsWonderGateDevice::OnSend(uint8_t value, uint64_t masterClock)
{
	_lastSendClock = masterClock;
	wg_rx(&_gate, value);
}

bool WsWonderGateDevice::Receive(uint8_t& value, uint64_t masterClock)
{
	if(_gate.rx_state != 0 && masterClock - _lastSendClock > FrameGapClocks) {
		wg_rx_reset(&_gate);
	}

	if(_toConsole.empty() || masterClock - _lastSendClock < QuietLineClocks) {
		return false;
	}
	value = _toConsole.front();
	_toConsole.pop_front();
	return true;
}

WsWonderGateDevice::Connection* WsWonderGateDevice::Get(int h)
{
	if(h < 0 || h >= (int)(sizeof(_connections) / sizeof(_connections[0])) || !_connections[h].Sock) {
		return nullptr;
	}
	return &_connections[h];
}

void WsWonderGateDevice::Tx(void* ctx, const uint8_t* buf, size_t len)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	self->_toConsole.insert(self->_toConsole.end(), buf, buf + len);
}

int WsWonderGateDevice::Resolve(void* ctx, const char* name, uint8_t ip[4])
{
	//Every name goes to the relay
	ip[0] = 127;
	ip[1] = 0;
	ip[2] = 0;
	ip[3] = 1;
	return 0;
}

int WsWonderGateDevice::SockOpen(void* ctx)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	for(int i = 0; i < (int)(sizeof(self->_connections) / sizeof(self->_connections[0])); i++) {
		if(!self->_connections[i].Sock) {
			self->_connections[i].Sock.reset(new Socket());
			self->_connections[i].AwaitingReply = false;
			return i;
		}
	}
	return -1;
}

int WsWonderGateDevice::SockConnect(void* ctx, int h, const uint8_t ip[4], uint16_t port)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	Connection* c = self->Get(h);
	if(!c) {
		return -1;
	}

	//Every connection goes to the relay, which learns the game's port from the preamble
	//(HTTP 80, Wonder Classic 10000, the browser's BPL check 5555, mail 25/110, ...)
	if(self->_relayPort == 0 || !c->Sock->Connect("127.0.0.1", self->_relayPort) || c->Sock->ConnectionError()) {
		return -1;
	}
	char preamble[4] = { 'W', 'G', (char)(port >> 8), (char)(port & 0xFF) };
	c->Sock->Send(preamble, sizeof(preamble), 0);
	//Some servers speak first (Wonder Classic on 10000, mail): a game that receives before sending
	//is waiting for that greeting, which has to come through the tunnel
	c->AwaitingReply = true;
	return c->Sock->ConnectionError() ? -1 : 0;
}

int WsWonderGateDevice::SockSend(void* ctx, int h, const uint8_t* buf, size_t len)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	Connection* c = self->Get(h);
	if(!c || c->Sock->ConnectionError()) {
		return -1;
	}
	c->Sock->Send((char*)buf, (int)len, 0);
	if(c->Sock->ConnectionError()) {
		return -1;
	}
	c->AwaitingReply = true;
	return (int)len;
}

int WsWonderGateDevice::SockRecv(void* ctx, int h, uint8_t* buf, size_t len)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	Connection* c = self->Get(h);
	if(!c) {
		return 0;
	}

	auto start = std::chrono::steady_clock::now();
	while(true) {
		int n = c->Sock->Recv((char*)buf, (int)len, 0);
		if(n > 0) {
			c->AwaitingReply = false;
			return n;
		}
		if(n == 0 || c->Sock->ConnectionError()) {
			return 0; //closed
		}

		//Nothing yet: wait only if the game is expecting an answer to what it sent
		auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		if(!c->AwaitingReply || waited >= ReplyWaitMs) {
			return WG_RECV_NODATA;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void WsWonderGateDevice::SockShutdown(void* ctx, int h, int how)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	if(Connection* c = self->Get(h)) {
		c->Sock->Shutdown(how != 0, how != 1);
	}
}

void WsWonderGateDevice::SockClose(void* ctx, int h)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	if(Connection* c = self->Get(h)) {
		c->Sock.reset();
		c->AwaitingReply = false;
	}
}

void WsWonderGateDevice::Log(void* ctx, const char* line)
{
	WsWonderGateDevice* self = (WsWonderGateDevice*)ctx;
	if(!self->_trace.is_open()) {
		return;
	}
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	std::tm tm = {};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char ts[32];
	std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
	self->_trace << ts << line << std::endl;
}
