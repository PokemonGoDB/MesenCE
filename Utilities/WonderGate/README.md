# wgcore

MobileWonderGate adapter emulation, used by the WonderSwan core's WonderGate device
(`Core/WS/WsWonderGateDevice.cpp`).

`wg.c` and `wg.h` are copied unchanged from the Swan Revival game server project
(`wondergate/core/` in WonderswanCartDumper), where the adapter protocol is worked out and tested
against each game. Update them by copying again rather than editing here. They are included in
MesenCE under its GPLv3 license. Protocol knowledge comes from trap15's wonderfence (MIT,
(C) 2014 Alex Marshall), the WSdev wiki, and facts (not code) from Yuvi's
[WonderGateway](https://github.com/Yuvi-App/WonderGateway) (Keitai Archive): three replies from its
real-adapter capture (`55 55 55 55 55` on wake-up, `0F 00 01 FF` at power-off, `0B 02` for a busy
line) and four per-game quirks from its testing (socket numbers from 0, Buffers Evolution's "ok"
phone status, Rainbow Islands' 2-byte close reply, Raku Jongg's 4-byte hang-up reply).
